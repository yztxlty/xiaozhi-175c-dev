#include "role_animation_store.h"

#include "board.h"
#include "device_content/role_visual_store.h"
#include "display.h"
#include "settings.h"
#include "storage/content_storage.h"
#include "system_info.h"

#include <esp_log.h>
#include <cJSON.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#define TAG "RoleAnimationStore"

namespace {
constexpr size_t kMaxFileBytes = 512 * 1024;
constexpr size_t kMaxTotalBytes = 5 * kMaxFileBytes;

std::string DigestHex(const unsigned char digest[32]) {
    char value[65] = {};
    for (size_t i = 0; i < 32; ++i)
        snprintf(value + i * 2, 3, "%02x", digest[i]);
    return value;
}

bool SafeId(const std::string& value) {
    return !value.empty() && value.size() <= 96 &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == ':';
           });
}

bool SupportedAction(const std::string& action) {
    return action == "idle" || action == "faint" || action == "listening" ||
           action == "speaking" || action == "thinking";
}

bool Download(const std::string& url, const std::string& path, const std::string& sha256,
              size_t expected_bytes) {
    if (url.rfind("https://", 0) != 0 || sha256.size() != 64 || expected_bytes == 0 ||
        expected_bytes > kMaxFileBytes)
        return false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(90000);
    http->SetHeader("Device-Id", SystemInfo::GetDeviceId().c_str());
    if (!http->Open("GET", url) || http->GetStatusCode() != 200 ||
        http->GetBodyLength() != expected_bytes) {
        ESP_LOGE(TAG, "Animation download rejected: status=%d body=%u expected=%u",
                 http->GetStatusCode(), static_cast<unsigned>(http->GetBodyLength()),
                 static_cast<unsigned>(expected_bytes));
        http->Close();
        return false;
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        http->Close();
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    unsigned char head[4] = {};
    char buffer[4096];
    size_t total = 0;
    bool ok = true;
    while (total < expected_bytes) {
        const int read = http->Read(buffer, std::min(sizeof(buffer), expected_bytes - total));
        if (read <= 0 || fwrite(buffer, 1, read, file) != static_cast<size_t>(read)) {
            ESP_LOGE(TAG, "Animation download stopped: read=%d received=%u expected=%u",
                     read, static_cast<unsigned>(total), static_cast<unsigned>(expected_bytes));
            ok = false;
            break;
        }
        if (total < sizeof(head))
            memcpy(head + total, buffer, std::min<size_t>(read, sizeof(head) - total));
        mbedtls_sha256_update(&sha, reinterpret_cast<unsigned char*>(buffer), read);
        total += read;
    }
    ok = ok && total == expected_bytes && head[0] == 0x89 && memcmp(head + 1, "EAF", 3) == 0;
    ok = ok && fflush(file) == 0;
    fclose(file);
    http->Close();
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    std::string expected = sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
    if (!ok || DigestHex(digest) != expected)
        ESP_LOGE(TAG, "Animation file validation failed: bytes=%u expected=%u",
                 static_cast<unsigned>(total), static_cast<unsigned>(expected_bytes));
    return ok && DigestHex(digest) == expected;
}

std::string CanonicalManifestSha256(const cJSON* manifest) {
    cJSON* canonical = cJSON_CreateObject();
    cJSON_AddNumberToObject(canonical, "protocolVersion",
                            cJSON_GetObjectItem(manifest, "protocolVersion")->valueint);
    cJSON_AddStringToObject(canonical, "roleId",
                            cJSON_GetObjectItem(manifest, "roleId")->valuestring);
    cJSON_AddNumberToObject(
        canonical, "expectedConfigurationRevision",
        cJSON_GetObjectItem(manifest, "expectedConfigurationRevision")->valueint);
    cJSON_AddNumberToObject(canonical, "revision",
                            cJSON_GetObjectItem(manifest, "revision")->valueint);
    cJSON* actions = cJSON_AddArrayToObject(canonical, "actions");
    std::vector<const cJSON*> sorted;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach (item, cJSON_GetObjectItem(manifest, "actions"))
        sorted.push_back(item);
    std::sort(sorted.begin(), sorted.end(), [](const cJSON* left, const cJSON* right) {
        return strcmp(cJSON_GetObjectItem(left, "actionCode")->valuestring,
                      cJSON_GetObjectItem(right, "actionCode")->valuestring) < 0;
    });
    constexpr const char* fields[] = {"actionCode", "assetId", "assetVersion", "fileName",
                                      "format",     "sha256",  "sizeBytes",    "width",
                                      "height",     "fps",     "playMode"};
    for (const cJSON* source : sorted) {
        cJSON* row = cJSON_CreateObject();
        for (const char* field : fields)
            cJSON_AddItemToObject(row, field,
                                  cJSON_Duplicate(cJSON_GetObjectItem(source, field), true));
        cJSON_AddItemToArray(actions, row);
    }
    char* text = cJSON_PrintUnformatted(canonical);
    cJSON_Delete(canonical);
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(text), strlen(text), digest, 0);
    cJSON_free(text);
    return DigestHex(digest);
}
}  // namespace

RoleAnimationStore& RoleAnimationStore::GetInstance() {
    static RoleAnimationStore instance;
    return instance;
}

std::string RoleAnimationStore::ManifestPath(const std::string& slot) const {
    return ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery,
                                                 "role_animation_" + slot + ".jsn");
}

bool RoleAnimationStore::Load() {
    const std::string slot = Settings("companion", false).GetString("role_anim_slot");
    if (slot != "a" && slot != "b")
        return true;
    FILE* file = fopen(ManifestPath(slot).c_str(), "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file)
            fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size <= 0 || size > 8192 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    std::string json(static_cast<size_t>(size), '\0');
    const bool read_ok = fread(json.data(), 1, json.size(), file) == json.size();
    fclose(file);
    cJSON* root = read_ok ? cJSON_ParseWithLength(json.data(), json.size()) : nullptr;
    if (!root)
        return false;
    auto role = cJSON_GetObjectItem(root, "roleId");
    auto config = cJSON_GetObjectItem(root, "expectedConfigurationRevision");
    auto revision = cJSON_GetObjectItem(root, "revision");
    auto digest = cJSON_GetObjectItem(root, "manifestSha256");
    auto request = cJSON_GetObjectItem(root, "lastRequestId");
    auto actions = cJSON_GetObjectItem(root, "actions");
    if (!cJSON_IsString(role) || !cJSON_IsNumber(config) || !cJSON_IsNumber(revision) ||
        !cJSON_IsString(digest) || !cJSON_IsString(request) || !cJSON_IsArray(actions)) {
        cJSON_Delete(root);
        return false;
    }
    std::vector<Item> loaded;
    bool valid = cJSON_GetArraySize(actions) >= 1 && cJSON_GetArraySize(actions) <= 5;
    cJSON* row = nullptr;
    cJSON_ArrayForEach (row, actions) {
        auto action = cJSON_GetObjectItem(row, "actionCode");
        auto asset = cJSON_GetObjectItem(row, "assetId");
        auto version = cJSON_GetObjectItem(row, "assetVersion");
        auto name = cJSON_GetObjectItem(row, "storedName");
        auto sha = cJSON_GetObjectItem(row, "sha256");
        auto bytes = cJSON_GetObjectItem(row, "sizeBytes");
        if (!cJSON_IsString(action) || !cJSON_IsString(asset) || !cJSON_IsNumber(version) ||
            !cJSON_IsString(name) || !ContentStorage::IsSafeFileName(name->valuestring) ||
            !cJSON_IsString(sha) || !cJSON_IsNumber(bytes)) {
            valid = false;
            break;
        }
        const std::string path = ContentStorage::GetInstance().GetPath(
            ContentStorage::Category::Gallery, name->valuestring);
        FILE* asset_file = fopen(path.c_str(), "rb");
        if (!asset_file) {
            valid = false;
            break;
        }
        fclose(asset_file);
        loaded.push_back({action->valuestring, asset->valuestring, version->valueint, path,
                          sha->valuestring, static_cast<size_t>(bytes->valuedouble)});
    }
    if (!valid || loaded.size() != static_cast<size_t>(cJSON_GetArraySize(actions))) {
        cJSON_Delete(root);
        return false;
    }
    role_id_ = role->valuestring;
    configuration_revision_ = config->valueint;
    revision_ = revision->valueint;
    manifest_sha256_ = digest->valuestring;
    last_request_id_ = request->valuestring;
    items_ = std::move(loaded);
    cJSON_Delete(root);
    return true;
}

bool RoleAnimationStore::SaveManifest(const std::string& slot, const cJSON* manifest,
                                      const std::vector<Item>& items) const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "roleId", cJSON_GetObjectItem(manifest, "roleId")->valuestring);
    cJSON_AddNumberToObject(
        root, "expectedConfigurationRevision",
        cJSON_GetObjectItem(manifest, "expectedConfigurationRevision")->valueint);
    cJSON_AddNumberToObject(root, "revision", cJSON_GetObjectItem(manifest, "revision")->valueint);
    cJSON_AddStringToObject(root, "manifestSha256",
                            cJSON_GetObjectItem(manifest, "manifestSha256")->valuestring);
    cJSON_AddStringToObject(root, "lastRequestId", last_request_id_.c_str());
    cJSON* actions = cJSON_AddArrayToObject(root, "actions");
    for (const auto& item : items) {
        cJSON* row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "actionCode", item.action_code.c_str());
        cJSON_AddStringToObject(row, "assetId", item.asset_id.c_str());
        cJSON_AddNumberToObject(row, "assetVersion", item.asset_version);
        cJSON_AddStringToObject(row, "storedName",
                                item.path.substr(item.path.find_last_of('/') + 1).c_str());
        cJSON_AddStringToObject(row, "sha256", item.sha256.c_str());
        cJSON_AddNumberToObject(row, "sizeBytes", static_cast<double>(item.bytes));
        cJSON_AddItemToArray(actions, row);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
        return false;
    const std::string path = ManifestPath(slot);
    const std::string temporary = path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    const size_t length = strlen(json);
    const bool ok = file && fwrite(json, 1, length, file) == length && fflush(file) == 0;
    if (file)
        fclose(file);
    cJSON_free(json);
    if (!ok)
        return false;
    std::remove(path.c_str());
    return std::rename(temporary.c_str(), path.c_str()) == 0;
}

bool RoleAnimationStore::Apply(const cJSON* params, const std::string& request_id) {
    const cJSON* manifest =
        cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "manifest") : nullptr;
    if (!cJSON_IsObject(manifest))
        manifest = params;
    auto protocol = cJSON_GetObjectItem(manifest, "protocolVersion");
    auto role = cJSON_GetObjectItem(manifest, "roleId");
    auto config = cJSON_GetObjectItem(manifest, "expectedConfigurationRevision");
    auto base = cJSON_GetObjectItem(manifest, "baseRevision");
    auto revision = cJSON_GetObjectItem(manifest, "revision");
    auto expected_sha = cJSON_GetObjectItem(manifest, "manifestSha256");
    auto actions = cJSON_GetObjectItem(manifest, "actions");
    Settings companion("companion", false);
    if (!cJSON_IsNumber(protocol) || protocol->valueint != 1 || !cJSON_IsString(role) ||
        !SafeId(role->valuestring) || !cJSON_IsNumber(config) || !cJSON_IsNumber(base) ||
        !cJSON_IsNumber(revision) || !cJSON_IsString(expected_sha) ||
        strlen(expected_sha->valuestring) != 64 || !cJSON_IsArray(actions) ||
        strcmp(role->valuestring, companion.GetString("active_role").c_str()) != 0 ||
        // Cloud-only model updates can advance the server revision without changing device config.
        config->valueint < companion.GetInt("cfg_rev", 0))
        return false;
    if (request_id == last_request_id_)
        return manifest_sha256_ == expected_sha->valuestring;
    if (base->valueint != revision_)
        return false;
    const int count = cJSON_GetArraySize(actions);
    if (count < 1 || count > 5)
        return false;

    const std::vector<Item> previous_items = items_;
    std::vector<Item> incoming_actions;
    std::vector<std::string> staged;
    auto rollback = [&staged, &previous_items]() {
        for (const auto& path : staged) {
            const bool was_active =
                std::any_of(previous_items.begin(), previous_items.end(),
                            [&](const Item& item) { return item.path == path; });
            if (!was_active)
                std::remove(path.c_str());
        }
        return false;
    };
    size_t total = 0;
    cJSON* row = nullptr;
    cJSON_ArrayForEach (row, actions) {
        auto action = cJSON_GetObjectItem(row, "actionCode");
        auto asset = cJSON_GetObjectItem(row, "assetId");
        auto asset_version = cJSON_GetObjectItem(row, "assetVersion");
        auto file_name = cJSON_GetObjectItem(row, "fileName");
        auto format = cJSON_GetObjectItem(row, "format");
        auto url = cJSON_GetObjectItem(row, "url");
        auto sha = cJSON_GetObjectItem(row, "sha256");
        auto bytes = cJSON_GetObjectItem(row, "sizeBytes");
        auto width = cJSON_GetObjectItem(row, "width");
        auto height = cJSON_GetObjectItem(row, "height");
        auto fps = cJSON_GetObjectItem(row, "fps");
        auto play_mode = cJSON_GetObjectItem(row, "playMode");
        if (!cJSON_IsString(action) || !SupportedAction(action->valuestring) ||
            !cJSON_IsString(asset) || !SafeId(asset->valuestring) ||
            !cJSON_IsNumber(asset_version) || asset_version->valueint < 1 ||
            !cJSON_IsString(file_name) || !ContentStorage::IsSafeFileName(file_name->valuestring) ||
            std::string(file_name->valuestring) != std::string(action->valuestring) + ".eaf" ||
            !cJSON_IsString(format) || strcmp(format->valuestring, "eaf") != 0 ||
            !cJSON_IsString(url) || !cJSON_IsString(sha) || strlen(sha->valuestring) != 64 ||
            !cJSON_IsNumber(bytes) || !cJSON_IsNumber(width) || width->valueint != 466 ||
            !cJSON_IsNumber(height) || height->valueint != 466 || !cJSON_IsNumber(fps) ||
            fps->valueint < 1 || fps->valueint > 10 || !cJSON_IsString(play_mode) ||
            strcmp(play_mode->valuestring, "LOOP") != 0 ||
            std::any_of(incoming_actions.begin(), incoming_actions.end(),
                        [&](const Item& item) { return item.action_code == action->valuestring; }))
            return rollback();
        const size_t size = static_cast<size_t>(bytes->valuedouble);
        total += size;
        if (size == 0 || size > kMaxFileBytes || total > kMaxTotalBytes)
            return rollback();
        const std::string stored_name = std::string("ra_") + std::string(sha->valuestring, 16) +
                                        "_" + action->valuestring + ".eaf";
        const std::string path =
            ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, stored_name);
        const std::string temporary = path + ".tmp";
        if (path.empty() ||
            !ContentStorage::GetInstance().CanReserve(ContentStorage::Category::Gallery, size) ||
            !Download(url->valuestring, temporary, sha->valuestring, size)) {
            ESP_LOGE(TAG, "Animation action staging failed: action=%s galleryFree=%u",
                     action->valuestring,
                     static_cast<unsigned>(ContentStorage::GetInstance().GetStats().gallery_free_bytes));
            if (!temporary.empty())
                std::remove(temporary.c_str());
            return rollback();
        }
        std::remove(path.c_str());
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            std::remove(temporary.c_str());
            return rollback();
        }
        staged.push_back(path);
        if (!Board::GetInstance().GetDisplay()->ValidateRoleImage(path.c_str(), "eaf")) {
            ESP_LOGE(TAG, "Animation display validation failed: action=%s", action->valuestring);
            return rollback();
        }
        incoming_actions.push_back({action->valuestring, asset->valuestring,
                                    asset_version->valueint, path, sha->valuestring, size});
    }
    if (CanonicalManifestSha256(manifest) != expected_sha->valuestring)
        return rollback();
    const std::string active_slot = companion.GetString("role_anim_slot", "a");
    const std::string inactive_slot = active_slot == "a" ? "b" : "a";
    last_request_id_ = request_id;
    if (!SaveManifest(inactive_slot, manifest, incoming_actions))
        return rollback();
    Settings writable("companion", true);
    writable.SetString("role_anim_slot", inactive_slot);
    role_id_ = role->valuestring;
    configuration_revision_ = config->valueint;
    revision_ = revision->valueint;
    manifest_sha256_ = expected_sha->valuestring;
    items_ = std::move(incoming_actions);
    for (const auto& previous : previous_items) {
        const bool retained = std::any_of(items_.begin(), items_.end(), [&](const Item& item) {
            return item.path == previous.path;
        });
        if (!retained)
            std::remove(previous.path.c_str());
    }
    Resume();
    return true;
}

bool RoleAnimationStore::Show(const std::string& action_code) {
    const auto item = std::find_if(items_.begin(), items_.end(), [&](const Item& value) {
        return value.action_code == action_code;
    });
    if (item == items_.end())
        return false;
    if (current_action_ == action_code)
        return true;
    if (!Board::GetInstance().GetDisplay()->SetRoleImage(item->path.c_str(), "eaf"))
        return false;
    current_action_ = action_code;
    return true;
}

void RoleAnimationStore::Resume() {
    const std::string visible_action = current_action_;
    current_action_.clear();
    if (!visible_action.empty() && Show(visible_action))
        return;
    RoleVisualStore::GetInstance().LoadActive();
}

void RoleAnimationStore::Restore() {
    if (current_action_.empty())
        return;
    current_action_.clear();
    if (Settings("companion", false).GetString("active_slot").empty()) {
        Board::GetInstance().GetDisplay()->SetRoleImage("");
    } else {
        RoleVisualStore::GetInstance().LoadActive();
    }
}

void RoleAnimationStore::ClearForRoleChange(const std::string& role_id) {
    if (!role_id_.empty() && role_id_ == role_id)
        return;
    DIR* directory = opendir("/gallery");
    if (directory != nullptr) {
        while (const auto* entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name.rfind("ra_", 0) == 0 && name.size() > 7 &&
                name.substr(name.size() - 4) == ".eaf") {
                const auto path = ContentStorage::GetInstance().GetPath(
                    ContentStorage::Category::Gallery, name);
                if (!path.empty()) std::remove(path.c_str());
            }
        }
        closedir(directory);
    }
    Settings("companion", true).EraseKey("role_anim_slot");
    role_id_.clear();
    revision_ = 0;
    configuration_revision_ = 0;
    manifest_sha256_.clear();
    last_request_id_.clear();
    items_.clear();
    Restore();
}

void RoleAnimationStore::Reset() {
    Settings("companion", true).EraseKey("role_anim_slot");
    role_id_.clear();
    revision_ = 0;
    configuration_revision_ = 0;
    manifest_sha256_.clear();
    last_request_id_.clear();
    current_action_.clear();
    items_.clear();
    Resume();
}

void RoleAnimationStore::AddReported(cJSON* reported, const std::string& request_id) const {
    const auto storage = ContentStorage::GetInstance().GetStats();
    cJSON_AddNumberToObject(reported, "roleAnimationGalleryUsedBytes", storage.gallery_bytes);
    cJSON_AddNumberToObject(reported, "roleAnimationGalleryFreeBytes", storage.gallery_free_bytes);
    cJSON_AddNumberToObject(reported, "roleAnimationRevision", revision_);
    cJSON_AddStringToObject(reported, "roleAnimationManifestSha256", manifest_sha256_.c_str());
    cJSON_AddStringToObject(reported, "lastRoleAnimationRequestId",
                            request_id.empty() ? last_request_id_.c_str() : request_id.c_str());
    cJSON* actions = cJSON_AddArrayToObject(reported, "actions");
    for (const auto& item : items_) {
        cJSON* row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "actionCode", item.action_code.c_str());
        cJSON_AddStringToObject(row, "assetId", item.asset_id.c_str());
        cJSON_AddNumberToObject(row, "assetVersion", item.asset_version);
        cJSON_AddItemToArray(actions, row);
    }
}
