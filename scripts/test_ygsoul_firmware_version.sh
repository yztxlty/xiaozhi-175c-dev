#!/usr/bin/env bash
set -euo pipefail

grep -q 'set(PROJECT_VER "1.0.5")' CMakeLists.txt
grep -q 'ReportDeviceUplink' main/application.cc
grep -q '"reportType"' main/application.cc
grep -q 'unsupported_command' main/application.cc
grep -q 'reportUrl' main/ota.cc
grep -q 'ReportProgress' main/ota.cc
grep -q 'mbedtls_sha256' main/ota.cc
grep -q 'device_management_client.cc' main/CMakeLists.txt
grep -q 'DeviceManagementClient' main/application.h
grep -q 'Settings settings("management"' main/ota.cc
grep -q 'device_mgmt' main/device_management_client.cc
grep -q 'heartbeat' main/device_management_client.cc
grep -Fq 'if (!client->Send("{\"type\":\"heartbeat\"}"))' main/device_management_client.cc
grep -Fq 'client->connected_ = false;' main/device_management_client.cc
grep -q 'OnMessage' main/device_management_client.cc
grep -q 'HandleCustomMessage' main/application.cc
grep -q 'Reporting online immediately after Wi-Fi join' main/application.cc
grep -q 'Wi-Fi rejoined, keep existing management session' main/application.cc
grep -q 'strcmp(command->valuestring, "unbind") == 0' main/application.cc
grep -q 'strcmp(command->valuestring, "factoryReset") == 0' main/application.cc
grep -q 'cJSON_AddBoolToObject(reported, "unbound", true)' main/application.cc
grep -q 'cJSON_AddBoolToObject(reported, "factoryReset", true)' main/application.cc
grep -q 'CONFIG_OTA_URL="https://yomitest.gwcz.online/ydp/v1/ota/xiaozhi/check"' sdkconfig.175c
! grep -q 'api.tenclass.net' sdkconfig.175c
echo "YGSoul firmware version contract passed"
