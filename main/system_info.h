#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>
#include <array>
#include <cstdint>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

class SystemInfo {
public:
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
    static std::string GetMacAddress();
    static std::string GetDeviceId();
    static int GetAuthKeySlot();
    static bool GetFlashAuthKey(std::array<uint8_t, 32>& key);
    static bool FactoryProofPending();
    static bool ClearFactoryProofPending();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    static void PrintTaskList();
    static void PrintHeapStats();
    static void PrintPmLocks();
};

#endif // _SYSTEM_INFO_H_
