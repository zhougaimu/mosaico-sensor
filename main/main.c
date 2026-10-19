#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

static const char *TAG = "mosaico";

#define WIFI_MAX_RETRY 10
#define AP_SSID "mosaico-diag"
#define NVS_NS "wifi"

static int s_retry_count = 0;
static bool s_sta_connected = false;
static bool s_give_up = false;
static uint8_t s_last_reason = 0;
static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static int s_fake_seq = 0;
static bool s_ap_started = false;
static httpd_handle_t s_server = NULL;

static void usb_console_init(void)
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = { 0 };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
}

static void load_credentials(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_ssid);
    bool have = false;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "ssid", s_ssid, &len) == ESP_OK) {
            len = sizeof(s_pass);
            nvs_get_str(h, "pass", s_pass, &len);
            have = true;
        }
        nvs_close(h);
    }

    if (!have) {
        strlcpy(s_ssid, CONFIG_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, CONFIG_WIFI_PASSWORD, sizeof(s_pass));
    }
    ESP_LOGI(TAG, "using ssid=%s", s_ssid);
}

static void save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_str(h, "ssid", ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "pass", pass));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

static void url_decode(char *dst, const char *src, size_t dstlen)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstlen; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = 0;
}

static const char *reason_text(uint8_t r)
{
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE: return "auth expired / wrong password";
    case WIFI_REASON_NO_AP_FOUND: return "AP not found (SSID wrong or out of range)";
    case WIFI_REASON_AUTH_FAIL: return "authentication failed (password?)";
    case WIFI_REASON_ASSOC_FAIL: return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake timeout";
    default: return "other";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        s_last_reason = d->reason;
        s_sta_connected = false;
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "wifi disconnected (reason %d), retry %d/%d",
                     d->reason, s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "wifi connect failed (reason %d, %s), entering diag mode",
                     d->reason, reason_text(d->reason));
            s_give_up = true;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_sta_connected = true;
    }
}

static void start_ap(void)
{
    if (s_ap_started) {
        return;
    }
    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_started = true;
    ESP_LOGI(TAG, "diag AP started: ssid=%s http://192.168.4.1/", AP_SSID);
}

static esp_err_t root_get(httpd_req_t *req)
{
    char page[2200];
    int n = snprintf(page, sizeof(page),
        "<html><body><h2>mosaico-diag</h2>"
        "<p>STA: %s<br>SSID: %s<br>last reason: %d (%s)</p>"
        "<h3>visible APs</h3><pre>",
        s_sta_connected ? "connected" : "not connected",
        s_ssid, s_last_reason, reason_text(s_last_reason));

    uint16_t count = 0;
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
        if (count > 15) {
            count = 15;
        }
        if (count > 0) {
            wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
            if (recs) {
                esp_wifi_scan_get_ap_records(&count, recs);
                for (int i = 0; i < count && n < (int)sizeof(page) - 80; i++) {
                    n += snprintf(page + n, sizeof(page) - n, "%s  rssi=%d  ch=%d\\n",
                                  (char *)recs[i].ssid, recs[i].rssi, recs[i].primary);
                }
                free(recs);
            }
        }
    } else {
        n += snprintf(page + n, sizeof(page) - n, "(scan error %d)\n", err);
    }

    n += snprintf(page + n, sizeof(page) - n,
        "</pre><h3>set wifi</h3>"
        "<form method=\"POST\" action=\"/config\">"
        "SSID: <input name=\"ssid\" size=\"24\"><br>"
        "PASS: <input name=\"pass\" type=\"password\" size=\"24\"><br>"
        "<button>save and reboot</button></form></body></html>");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, n);
}

static esp_err_t config_post(httpd_req_t *req)
{
    char buf[256];
    char ssid[33] = {0};
    char pass[65] = {0};
    int len = req->content_len < (int)sizeof(buf) - 1 ? req->content_len : (int)sizeof(buf) - 1;
    int r = httpd_req_recv(req, buf, len);
    if (r <= 0) {
        return ESP_FAIL;
    }
    buf[r] = 0;

    char *save = NULL;
    for (char *kv = strtok_r(buf, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char val[65];
        url_decode(val, eq + 1, sizeof(val));
        if (strcmp(kv, "ssid") == 0) {
            strlcpy(ssid, val, sizeof(ssid));
        } else if (strcmp(kv, "pass") == 0) {
            strlcpy(pass, val, sizeof(pass));
        }
    }

    if (ssid[0]) {
        save_credentials(ssid, pass);
        httpd_resp_sendstr(req, "saved, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return httpd_resp_sendstr(req, "missing ssid");
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        s_server = NULL;
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t conf = { .uri = "/config", .method = HTTP_POST, .handler = config_post };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &conf);
    ESP_LOGI(TAG, "diag http server on port 80");
}

static void report(const char *body)
{
    esp_http_client_config_t cfg = {
        .url = CONFIG_REPORT_URL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "report %s -> %d", body, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "report failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi sta started");
}

void app_main(void)
{
    usb_console_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_credentials();
    wifi_start();

    while (1) {
        if (s_sta_connected) {
            s_fake_seq++;
            char body[160];
            snprintf(body, sizeof(body), "{\"raw\":%d,\"uptime_ms\":%lld}",
                     s_fake_seq, (long long)(esp_timer_get_time() / 1000));
            report(body);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_REPORT_INTERVAL_S * 1000));
        } else {
            if (s_give_up && !s_ap_started) {
                start_ap();
            }
            if (s_give_up && !s_server) {
                start_http();
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
