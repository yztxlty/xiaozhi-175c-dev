#include "watch_face_store.h"

#include "board.h"
#include "settings.h"
#include "storage/content_storage.h"

#include <cJSON.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "WatchFaceStore"

namespace {
constexpr size_t kMaxWatchFaceBytes = 512 * 1024;

std::string DigestHex(const unsigned char digest[32]) {
    char value[65] = {};
    for (size_t i = 0; i < 32; ++i) snprintf(value + i * 2, 3, "%02x", digest[i]);
    return value;
}

bool Download(const std::string& url, const std::string& path,
              const std::string& sha256, size_t expected_bytes) {
    if (url.rfind("https://", 0) != 0 || sha256.size() != 64 ||
        expected_bytes == 0 || expected_bytes > kMaxWatchFaceBytes) return false;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(20000);
    if (!http->Open("GET", url) || http->GetStatusCode() != 200 ||
        http->GetBodyLength() != expected_bytes) {
        http->Close();
        return false;
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) { http->Close(); return false; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    unsigned char head[3] = {};
    char buffer[4096];
    size_t total = 0;
    bool ok = true;
    while (total < expected_bytes) {
        const int read = http->Read(buffer, std::min(sizeof(buffer), expected_bytes - total));
        if (read <= 0 || fwrite(buffer, 1, read, file) != static_cast<size_t>(read)) { ok = false; break; }
        if (total < sizeof(head)) memcpy(head + total, buffer, std::min<size_t>(read, sizeof(head) - total));
        mbedtls_sha256_update(&sha, reinterpret_cast<unsigned char*>(buffer), read);
        total += read;
    }
    fflush(file);
    fclose(file);
    http->Close();
    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    std::string expected = sha256;
    std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
    return ok && total == expected_bytes && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff &&
           DigestHex(digest) == expected;
}
}  // namespace

WatchFaceStore& WatchFaceStore::GetInstance() {
    static WatchFaceStore instance;
    return instance;
}

std::string WatchFaceStore::SlotPath(const std::string& slot) const {
    return ContentStorage::GetInstance().GetPath(
        ContentStorage::Category::Gallery, slot == "a" ? "watch_face_a.jpg" : "watch_face_b.jpg");
}

bool WatchFaceStore::Apply(const cJSON* params) {
    auto manifest = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "manifest") : nullptr;
    auto resource = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "resourceId") : nullptr;
    auto version = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "contentVersion") : nullptr;
    auto type = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "resourceType") : nullptr;
    auto files = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "files") : nullptr;
    if (!cJSON_IsString(resource) || !cJSON_IsString(version) || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "WATCH_FACE") != 0 || !cJSON_IsArray(files) ||
        cJSON_GetArraySize(files) != 1) return false;
    auto file = cJSON_GetArrayItem(files, 0);
    auto url = cJSON_GetObjectItem(file, "url");
    auto sha = cJSON_GetObjectItem(file, "sha256");
    auto bytes = cJSON_GetObjectItem(file, "sizeBytes");
    auto format = cJSON_GetObjectItem(file, "format");
    auto width = cJSON_GetObjectItem(file, "width");
    auto height = cJSON_GetObjectItem(file, "height");
    if (!cJSON_IsString(url) || !cJSON_IsString(sha) || !cJSON_IsNumber(bytes) ||
        !cJSON_IsString(format) || strcmp(format->valuestring, "jpg") != 0 ||
        !cJSON_IsNumber(width) || width->valueint != 466 ||
        !cJSON_IsNumber(height) || height->valueint != 466) return false;

    Settings current("watch_face", false);
    if (current.GetString("resource") == resource->valuestring &&
        current.GetString("version") == version->valuestring) return true;
    const std::string old_slot = current.GetString("slot", "b");
    const std::string slot = old_slot == "a" ? "b" : "a";
    const std::string final_path = SlotPath(slot);
    const std::string temporary = final_path + ".tmp";
    const size_t size = static_cast<size_t>(bytes->valuedouble);
    if (!ContentStorage::GetInstance().CanReserve(ContentStorage::Category::Gallery, size) ||
        !Download(url->valuestring, temporary, sha->valuestring, size)) {
        std::remove(temporary.c_str());
        return false;
    }
    std::remove(final_path.c_str());
    if (std::rename(temporary.c_str(), final_path.c_str()) != 0) return false;
    Settings writable("watch_face", true);
    writable.SetString("slot", slot);
    writable.SetString("resource", resource->valuestring);
    writable.SetString("version", version->valuestring);
    writable.SetString("sha", sha->valuestring);
    writable.SetInt("bytes", static_cast<int>(size));
    if (!old_slot.empty() && old_slot != slot) std::remove(SlotPath(old_slot).c_str());
    ESP_LOGI(TAG, "Applied one watch face: bytes=%u", static_cast<unsigned>(size));
    return true;
}

std::string WatchFaceStore::CurrentPath() const {
    const std::string slot = Settings("watch_face", false).GetString("slot");
    if (slot.empty()) return {};
    const std::string path = SlotPath(slot);
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return {};
    fclose(file);
    return path;
}

std::string WatchFaceStore::ResourceId() const {
    return Settings("watch_face", false).GetString("resource");
}

std::string WatchFaceStore::ContentVersion() const {
    return Settings("watch_face", false).GetString("version");
}
