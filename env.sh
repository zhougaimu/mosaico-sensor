#!/bin/sh
# ESP-IDF 6.1's Python virtualenv is built with Python 3.12.
# Make sure `python3` resolves to 3.12 before sourcing export.sh.

python_is_312() {
    "$1" -c 'import sys; raise SystemExit(0 if sys.version_info[:2] == (3, 12) else 1)' >/dev/null 2>&1
}

PYTHON312=""
for candidate in \
    "$(command -v python3.12 2>/dev/null)" \
    /Library/Frameworks/Python.framework/Versions/3.12/bin/python3.12 \
    /opt/homebrew/opt/python@3.12/bin/python3.12 \
    /usr/local/opt/python@3.12/bin/python3.12
do
    if [ -n "$candidate" ] && [ -x "$candidate" ] && python_is_312 "$candidate"; then
        PYTHON312="$candidate"
        break
    fi
done

if [ -n "$PYTHON312" ]; then
    PY312_BIN_DIR="$(dirname "$PYTHON312")"
    if python_is_312 "$PY312_BIN_DIR/python3"; then
        PATH="$PY312_BIN_DIR:$PATH"
        export PATH
    else
        SHIM_DIR="${TMPDIR:-/tmp}/mosaico-idf-py312"
        mkdir -p "$SHIM_DIR"
        ln -sf "$PYTHON312" "$SHIM_DIR/python3"
        PATH="$SHIM_DIR:$PATH"
        export PATH
    fi
else
    echo "env.sh: Python 3.12 not found; ESP-IDF may fail to activate." >&2
fi

IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_PATH

. "$IDF_PATH/export.sh"
