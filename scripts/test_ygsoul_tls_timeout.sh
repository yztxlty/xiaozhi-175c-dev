#!/bin/sh
set -eu

grep -Fq 'cfg.timeout_ms = 30000;' components/78__esp-ml307/src/esp/esp_ssl.cc
