#include "role_visual_store.h"

#include "board.h"
#include "display.h"
#include "settings.h"
#include "storage/content_storage.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mbedtls/sha256.h>
#include <esp_log.h>

#define TAG "RoleVisualStore"

namespace {
constexpr size_t kMaxRoleImageBytes = 192 * 1024;

std::string DigestHex(const unsigned char digest[32]) {
    char value[65] = {};
    for (size_t i = 0; i < 32; ++i) {
        snprintf(value + i * 2, sizeof(value) - i * 2, "%02x", digest[i]);
    }
    return value;
}
}  // namespace

RoleVisualStore& RoleVisualStore::GetInstance() {
    static RoleVisualStore instance;
    return instance;
}

std::string RoleVisualStore::SlotPath(const std::string& slot) const {
    return ContentStorage::GetInstance().GetPath(
        ContentStorage::Category::SmallFiles,
        slot == "a" ? "role_a.jpg" : "role_b.jpg");
}

bool RoleVisualStore::Prepare(const std::string& resource_id, int version,
                              const std::string& url, const std::string& sha256,
                              size_t expected_bytes) {
    if (resource_id.empty() || version < 0 || sha256.size() != 64 ||
        expected_bytes == 0 || expected_bytes > kMaxRoleImageBytes ||
        url.rfind("https://", 0) != 0 ||
        ContentStorage::GetInstance().GetHealth() != ContentStorage::Health::Ready) {
        return false;
    }

    Settings settings("companion", false);
    const std::string slot = settings.GetString("active_slot", "a") == "a" ? "b" : "a";
    const std::string path = SlotPath(slot);
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(15000);
    if (!http->Open("GET", url) || http->GetStatusCode() != 200 ||
        http->GetBodyLength() != expected_bytes) {
        http->Close();
        return false;
    }

    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        http->Close();
        return false;
    }
    unsigned char digest[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    unsigned char first[2] = {};
    char buffer[4096];
    size_t total = 0;
    bool ok = true;
    while (total < expected_bytes) {
        const int read = http->Read(buffer, std::min(sizeof(buffer), expected_bytes - total));
        if (read <= 0 || fwrite(buffer, 1, read, file) != static_cast<size_t>(read)) {
            ok = false;
            break;
        }
        if (total < sizeof(first)) {
            memcpy(first + total, buffer, std::min<size_t>(read, sizeof(first) - total));
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<unsigned char*>(buffer), read);
        total += read;
    }
    ok = ok && total == expected_bytes && first[0] == 0xff && first[1] == 0xd8;
    fflush(file);
    fclose(file);
    http->Close();
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    std::string expected = sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
    if (!ok || DigestHex(digest) != expected) {
        ESP_LOGE(TAG, "Role image validation failed");
        return false;
    }

    Settings writable("companion", true);
    writable.SetString("prep_slot", slot);
    writable.SetString("prep_res", resource_id);
    writable.SetInt("prep_ver", version);
    ESP_LOGI(TAG, "Role image prepared: bytes=%u version=%d", static_cast<unsigned>(total), version);
    return true;
}

bool RoleVisualStore::Commit(const std::string& role_id, const std::string& resource_id,
                             int resource_version, int configuration_revision) {
    Settings current("companion", false);
    const int active_revision = current.GetInt("cfg_rev", 0);
    if (configuration_revision < active_revision) {
        return false;
    }
    if (configuration_revision == active_revision) {
        return current.GetString("active_role") == role_id &&
               current.GetString("role_res") == resource_id &&
               current.GetInt("role_ver", 0) == resource_version;
    }
    if (current.GetString("role_res") == resource_id &&
        current.GetInt("role_ver", 0) == resource_version) {
        Settings writable("companion", true);
        writable.SetString("active_role", role_id);
        writable.SetInt("cfg_rev", configuration_revision);
        return true;
    }
    if (resource_id == "BUILTIN_FALLBACK") {
        if (!Board::GetInstance().GetDisplay()->SetRoleImage("")) {
            return false;
        }
        Settings writable("companion", true);
        writable.SetString("active_role", role_id);
        writable.SetInt("cfg_rev", configuration_revision);
        writable.SetString("role_res", resource_id);
        writable.SetInt("role_ver", resource_version);
        writable.EraseKey("active_slot");
        writable.EraseKey("prep_slot");
        writable.EraseKey("prep_res");
        writable.EraseKey("prep_ver");
        std::remove(SlotPath("a").c_str());
        std::remove(SlotPath("b").c_str());
        return true;
    }
    const std::string slot = current.GetString("prep_slot");
    const std::string old_slot = current.GetString("active_slot");
    if (slot.empty() || current.GetString("prep_res") != resource_id ||
        current.GetInt("prep_ver", -1) != resource_version ||
        !Board::GetInstance().GetDisplay()->SetRoleImage(SlotPath(slot).c_str())) {
        return false;
    }

    Settings writable("companion", true);
    writable.SetString("active_role", role_id);
    writable.SetInt("cfg_rev", configuration_revision);
    writable.SetString("role_res", resource_id);
    writable.SetInt("role_ver", resource_version);
    writable.SetString("active_slot", slot);
    writable.EraseKey("prep_slot");
    writable.EraseKey("prep_res");
    writable.EraseKey("prep_ver");
    if (!old_slot.empty() && old_slot != slot) {
        std::remove(SlotPath(old_slot).c_str());
    }
    ESP_LOGI(TAG, "Role visual committed: revision=%d version=%d", configuration_revision, resource_version);
    return true;
}

void RoleVisualStore::LoadActive() {
    Settings settings("companion", false);
    const std::string slot = settings.GetString("active_slot");
    if (!slot.empty()) {
        Board::GetInstance().GetDisplay()->SetRoleImage(SlotPath(slot).c_str());
    }
}

void RoleVisualStore::Clear() {
    std::remove(SlotPath("a").c_str());
    std::remove(SlotPath("b").c_str());
    Board::GetInstance().GetDisplay()->SetRoleImage("");
    Settings("companion", true).EraseAll();
    ESP_LOGI(TAG, "Role configuration and visual cleared");
}
