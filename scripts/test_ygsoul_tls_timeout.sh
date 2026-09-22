#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_root"

fail() {
    printf '%s\n' "$1" >&2
    exit 1
}

grep -Fq 'cfg.timeout_ms = 8000;' components/78__esp-ml307/src/esp/esp_ssl.cc \
    || fail 'TLS 连接超时必须保持为 8000ms'

for component in components/78__esp-ml307 components/78__esp-wifi-connect; do
    grep -Fxq "!$component/**" .gitignore \
        || fail "固定依赖未完整排除出根目录忽略规则：$component"
done

for path in \
    components/78__esp-ml307/CMakeLists.txt \
    components/78__esp-ml307/src/esp/esp_ssl.cc \
    components/78__esp-ml307/src/web_socket.cc \
    components/78__esp-wifi-connect/CMakeLists.txt \
    components/78__esp-wifi-connect/wifi_manager.cc; do
    if git check-ignore -q -- "$path"; then
        fail "固定依赖仍被 Git 忽略：$path"
    fi
done

git check-ignore -q -- components/__ygsoul_unvendored_probe__/CMakeLists.txt \
    || fail '非固定 components 依赖必须继续被 Git 忽略'

grep -Fq '历史归档，不得用于恢复' scripts/补丁/TLS接收线程外部内存.patch \
    || fail '旧 TLS 补丁必须明确标记为不可恢复的历史归档'
if git apply --check -- scripts/补丁/TLS接收线程外部内存.patch >/dev/null 2>&1; then
    fail '历史归档不得继续作为可应用补丁'
fi
grep -Fq 'components/78__esp-ml307' README.md \
    || fail 'README 必须说明固定的 esp-ml307 依赖'
grep -Fq 'components/78__esp-wifi-connect' README.md \
    || fail 'README 必须说明固定的 esp-wifi-connect 依赖'
