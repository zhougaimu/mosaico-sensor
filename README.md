# mosaico-sensor

ESP-Mosaico（ESP32-S31）传感器采集 + WiFi 上报工程，基于 ESP-IDF v6.1。

## 功能

- WiFi STA 连接（断线自动重连，最多 10 次）
- ADC1 读取传感器原始值（默认 CH0，可在 menuconfig 改通道）
- 周期性 HTTP POST 上报 JSON：`{"raw":123,"uptime_ms":45678}`
- 上报间隔、服务器地址、WiFi 均通过 menuconfig 配置

## 环境搭建（首次）

```sh
mkdir -p ~/esp && cd ~/esp
git clone --depth 1 --branch v6.1 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s31
```

> 注意：本机代理（127.0.0.1:7897）存在 TLS 握手问题，git/网络操作前建议 `unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY`，或在代理恢复后使用。

## 日常开发

```sh
cd ~/esp-projects/mosaico-sensor
. ./env.sh                    # 激活 IDF 环境（每次新终端都要执行）
idf.py set-target esp32s31    # 仅首次需要，会生成 sdkconfig
idf.py menuconfig             # 配置 WiFi/服务器/间隔（Mosaico Sensor 菜单）
idf.py build
idf.py -p <串口设备> flash monitor    # 串口一般是 /dev/cu.usbmodem* 或 /dev/cu.usbserial*
```

## 上报协议

POST `CONFIG_REPORT_URL`（默认 `http://192.168.1.100:8000/api/sensor`），Content-Type: application/json：

```json
{"raw": 2048, "uptime_ms": 45678}
```

## 接入真实传感器

当前 `sensor_init()` / 主循环中是 ADC 占位读取。接入 I2C 传感器（如 SHT30/BME280）时：

1. 在 `main/CMakeLists.txt` 增加依赖（如 `espressif/bme280` 从 component registry：`idf.py add-dependency "espressif/bme280^3.0.0"`）
2. 在 `sensor_init()` 初始化 I2C 主机与传感器，主循环里替换 `adc_oneshot_read` 为真实读取
3. 扩展 `report()` 里的 JSON 字段
