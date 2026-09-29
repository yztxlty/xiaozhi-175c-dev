#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_pm.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <algorithm>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#define TAG "SystemInfo"

namespace {
constexpr char kFactoryPartition[] = "nvsfactory";

bool OpenFactoryNvs(nvs_handle_t& handle) {
    static const bool initialized = nvs_flash_init_partition(kFactoryPartition) == ESP_OK;
    return initialized && nvs_open_from_partition(kFactoryPartition, "factory", NVS_READONLY, &handle) == ESP_OK;
}

std::string FactoryString(const char* key) {
    nvs_handle_t handle = 0;
    if (!OpenFactoryNvs(handle)) return {};
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0 || size > 65) {
        nvs_close(handle);
        return {};
    }
    std::string value(size, '\0');
    const bool ok = nvs_get_str(handle, key, value.data(), &size) == ESP_OK;
    nvs_close(handle);
    if (!ok) return {};
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}
}

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    esp_wifi_get_mac(WIFI_IF_STA, mac);
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetDeviceId() {
    static const std::string device_id = [] {
        const std::string value = FactoryString("device_id");
        if (!value.empty() && value.size() <= 64 &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == ':' || c == '-';
            })) {
            return value;
        }
        return GetMacAddress();
    }();
    return device_id;
}

int SystemInfo::GetAuthKeySlot() {
    static const int slot = [] {
        const std::string storage = FactoryString("key_storage");
        if (storage == "flash") return -1;
        if (!storage.empty() && storage != "hardware") return -2;
        nvs_handle_t handle = 0;
        if (!OpenFactoryNvs(handle)) return 0;
        int32_t value = 0;
        const esp_err_t result = nvs_get_i32(handle, "key_slot", &value);
        nvs_close(handle);
        return result == ESP_ERR_NVS_NOT_FOUND ? 0 :
               result == ESP_OK && value >= 0 && value <= 5 ? static_cast<int>(value) : -2;
    }();
    return slot;
}

bool SystemInfo::GetFlashAuthKey(std::array<uint8_t, 32>& key) {
    if (GetAuthKeySlot() != -1) return false;
    nvs_handle_t handle = 0;
    if (!OpenFactoryNvs(handle)) return false;
    size_t size = key.size();
    const esp_err_t result = nvs_get_blob(handle, "auth_key", key.data(), &size);
    nvs_close(handle);
    return result == ESP_OK && size == key.size();
}

bool SystemInfo::FactoryProofPending() {
    nvs_handle_t handle = 0;
    if (!OpenFactoryNvs(handle)) return false;
    uint8_t pending = 0;
    const bool result = nvs_get_u8(handle, "proof_pending", &pending) == ESP_OK && pending == 1;
    nvs_close(handle);
    return result;
}

bool SystemInfo::ClearFactoryProofPending() {
    nvs_handle_t handle = 0;
    if (nvs_open_from_partition(kFactoryPartition, "factory", NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool result = nvs_set_u8(handle, "proof_pending", 0) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return result;
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "free sram: %u minimal sram: %u", free_sram, min_free_sram);
}

void SystemInfo::PrintPmLocks() {
    esp_pm_dump_locks(stdout);
}
