#include "gallery_store.h"

#include "board.h"
#include "storage/content_storage.h"

#include <cJSON.h>
#include <mbedtls/sha256.h>
#include <esp_log.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>

#define TAG "GalleryStore"

namespace {
constexpr size_t kMaxFileBytes = 2 * 1024 * 1024;
constexpr size_t kMaxTotalBytes = 6 * 1024 * 1024;
constexpr size_t kMaxStaticImages = 12;
constexpr size_t kMaxGifs = 3;

std::string DigestHex(const unsigned char digest[32]) {
    char value[65] = {};
    for (size_t i = 0; i < 32; ++i) snprintf(value + i * 2, 3, "%02x", digest[i]);
    return value;
}

std::string ItemFileName(const std::string& sha256, const std::string& item_id, const std::string& format) {
    unsigned char digest[32];
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(item_id.data()), item_id.size(), digest, 0);
    return sha256.substr(0, 12) + "_" + DigestHex(digest).substr(0, 8) +
           (format == "eaf" ? ".eaf" : format == "gif" ? ".gif" : ".jpg");
}

bool ExistingFileMatches(const GalleryStore::Item& item) {
    FILE* file = fopen(item.path.c_str(), "rb");
    if (!file) return false;
    const bool ok = fseek(file, 0, SEEK_END) == 0 && ftell(file) == static_cast<long>(item.bytes);
    fclose(file);
    return ok;
}

bool Download(const std::string& url, const std::string& path,
              const std::string& sha256, size_t expected_bytes,
              const std::string& format) {
    if (url.rfind("https://", 0) != 0 || sha256.size() != 64 ||
        expected_bytes == 0 || expected_bytes > kMaxFileBytes) {
        ESP_LOGE(TAG, "Invalid download metadata: bytes=%u sha_len=%u",
                 static_cast<unsigned>(expected_bytes), static_cast<unsigned>(sha256.size()));
        return false;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(20000);
    if (!http->Open("GET", url) || http->GetStatusCode() != 200 ||
        http->GetBodyLength() != expected_bytes) {
        ESP_LOGE(TAG, "HTTP metadata mismatch: status=%d body=%u expected=%u",
                 http->GetStatusCode(), static_cast<unsigned>(http->GetBodyLength()),
                 static_cast<unsigned>(expected_bytes));
        http->Close();
        return false;
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        ESP_LOGE(TAG, "Cannot open temporary file: %s errno=%d", path.c_str(), errno);
        http->Close();
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    unsigned char head[6] = {};
    char buffer[4096];
    size_t total = 0;
    bool ok = true;
    while (total < expected_bytes) {
        const int read = http->Read(buffer, std::min(sizeof(buffer), expected_bytes - total));
        if (read <= 0 || fwrite(buffer, 1, read, file) != static_cast<size_t>(read)) {
            ESP_LOGE(TAG, "Download write stopped: read=%d total=%u errno=%d",
                     read, static_cast<unsigned>(total), errno);
            ok = false;
            break;
        }
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
    const bool signature = format == "eaf"
        ? head[0] == 0x89 && (memcmp(head + 1, "EAF", 3) == 0 || memcmp(head + 1, "AAF", 3) == 0)
        : format == "gif"
            ? memcmp(head, "GIF87a", 6) == 0 || memcmp(head, "GIF89a", 6) == 0
            : head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff;
    const bool valid = ok && total == expected_bytes && signature && DigestHex(digest) == expected;
    if (!valid) {
        ESP_LOGE(TAG, "Downloaded file validation failed: total=%u expected=%u signature=%d hash=%d",
                 static_cast<unsigned>(total), static_cast<unsigned>(expected_bytes), signature,
                 DigestHex(digest) == expected);
    }
    return valid;
}
}  // namespace

GalleryStore& GalleryStore::GetInstance() {
    static GalleryStore instance;
    return instance;
}

bool GalleryStore::SafeId(const std::string& value) {
    return !value.empty() && value.size() <= 96 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == ':';
    });
}

bool GalleryStore::Load() {
    const std::string path = ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, "manifest.jsn");
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return true;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long size = ftell(file);
    if (size <= 0 || size > 32768 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
    std::string json(static_cast<size_t>(size), '\0');
    const bool read_ok = fread(json.data(), 1, json.size(), file) == json.size();
    fclose(file);
    if (!read_ok) return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (!root) return false;
    auto resource = cJSON_GetObjectItem(root, "resourceId");
    auto version = cJSON_GetObjectItem(root, "contentVersion");
    auto files = cJSON_GetObjectItem(root, "files");
    auto interval = cJSON_GetObjectItem(root, "intervalSec");
    auto loop = cJSON_GetObjectItem(root, "loop");
    std::vector<Item> loaded;
    cJSON* file_item = nullptr;
    cJSON_ArrayForEach(file_item, files) {
        auto id = cJSON_GetObjectItem(file_item, "itemId");
        auto name = cJSON_GetObjectItem(file_item, "fileName");
        auto format = cJSON_GetObjectItem(file_item, "format");
        auto sha = cJSON_GetObjectItem(file_item, "sha256");
        auto bytes = cJSON_GetObjectItem(file_item, "sizeBytes");
        if (cJSON_IsString(id) && cJSON_IsString(name) && cJSON_IsString(format) && cJSON_IsNumber(bytes)) {
            loaded.push_back({id->valuestring,
                ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, name->valuestring),
                format->valuestring, cJSON_IsString(sha) ? sha->valuestring : "",
                static_cast<size_t>(bytes->valuedouble)});
        }
    }
    if (cJSON_IsString(resource) && cJSON_IsString(version)) {
        std::lock_guard<std::mutex> lock(mutex_);
        resource_id_ = resource->valuestring;
        content_version_ = version->valuestring;
        interval_sec_ = cJSON_IsNumber(interval) ? std::max(3, std::min(interval->valueint, 60)) : 5;
        loop_ = !cJSON_IsBool(loop) || cJSON_IsTrue(loop);
        items_ = std::move(loaded);
    }
    cJSON_Delete(root);
    return true;
}

bool GalleryStore::Apply(const cJSON* params) {
    auto manifest = cJSON_IsObject(params) ? cJSON_GetObjectItem(params, "manifest") : nullptr;
    auto resource = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "resourceId") : nullptr;
    auto version = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "contentVersion") : nullptr;
    auto type = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "resourceType") : nullptr;
    auto files = cJSON_IsObject(manifest) ? cJSON_GetObjectItem(manifest, "files") : nullptr;
    if (!cJSON_IsString(resource) || !SafeId(resource->valuestring) || !cJSON_IsString(version) ||
        !cJSON_IsString(type) || !cJSON_IsArray(files)) return false;
    const int count = cJSON_GetArraySize(files);
    const bool gifs = strcmp(type->valuestring, "GIF") == 0;
    if (count <= 0) return false;
    std::vector<Item> next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        next = items_;
    }
    size_t total_bytes = 0;
    size_t static_count = 0;
    size_t gif_count = 0;
    for (const auto& item : next) {
        total_bytes += item.bytes;
        if (item.format == "gif" || item.format == "eaf") ++gif_count;
        else ++static_count;
    }
    std::vector<std::string> downloaded;
    std::vector<std::string> superseded;
    std::vector<std::string> incoming_ids;
    auto rollback = [&downloaded]() {
        for (const auto& path : downloaded) std::remove(path.c_str());
        return false;
    };
    cJSON* file_item = nullptr;
    cJSON_ArrayForEach(file_item, files) {
        auto id = cJSON_GetObjectItem(file_item, "itemId");
        auto url = cJSON_GetObjectItem(file_item, "url");
        auto sha = cJSON_GetObjectItem(file_item, "sha256");
        auto bytes = cJSON_GetObjectItem(file_item, "sizeBytes");
        auto format = cJSON_GetObjectItem(file_item, "format");
        if (!cJSON_IsString(id) || !SafeId(id->valuestring) || !cJSON_IsString(url) ||
            !cJSON_IsString(sha) || strlen(sha->valuestring) != 64 ||
            !cJSON_IsNumber(bytes) || !cJSON_IsString(format)) return rollback();
        if (std::find(incoming_ids.begin(), incoming_ids.end(), id->valuestring) != incoming_ids.end()) return rollback();
        incoming_ids.emplace_back(id->valuestring);
        const std::string kind = format->valuestring;
        if ((gifs && kind != "eaf" && kind != "gif") ||
            (!gifs && kind != "jpg" && kind != "jpeg")) return rollback();
        const size_t size = static_cast<size_t>(bytes->valuedouble);
        auto existing = std::find_if(next.begin(), next.end(), [&](const Item& item) {
            return item.item_id == id->valuestring;
        });
        if (existing != next.end() && existing->bytes == static_cast<size_t>(bytes->valuedouble) &&
            existing->format == kind && ExistingFileMatches(*existing)) {
            continue;
        }
        if (existing != next.end()) {
            total_bytes -= existing->bytes;
            if (existing->format == "gif" || existing->format == "eaf") --gif_count;
            else --static_count;
        }
        total_bytes += size;
        if (gifs) ++gif_count;
        else ++static_count;
        if (size == 0 || size > kMaxFileBytes || total_bytes > kMaxTotalBytes ||
            static_count > kMaxStaticImages || gif_count > kMaxGifs) return rollback();
        const std::string name = ItemFileName(sha->valuestring, id->valuestring, kind);
        const std::string path = ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, name);
        const std::string temporary = path.substr(0, path.find_last_of('.')) + ".tmp";
        if (path.empty() || !ContentStorage::GetInstance().CanReserve(ContentStorage::Category::Gallery, size)) {
            ESP_LOGE(TAG, "Gallery storage reserve failed: path=%s bytes=%u", path.c_str(), static_cast<unsigned>(size));
            return rollback();
        }
        if (!Download(url->valuestring, temporary, sha->valuestring, size, kind)) {
            if (!temporary.empty()) std::remove(temporary.c_str());
            return rollback();
        }
        std::remove(path.c_str());
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            ESP_LOGE(TAG, "Gallery rename failed: %s -> %s errno=%d", temporary.c_str(), path.c_str(), errno);
            if (!temporary.empty()) std::remove(temporary.c_str());
            return rollback();
        }
        downloaded.push_back(path);
        Item incoming{id->valuestring, path, kind, sha->valuestring, size};
        if (existing == next.end()) next.push_back(std::move(incoming));
        else {
            if (existing->path != path) superseded.push_back(existing->path);
            *existing = std::move(incoming);
        }
    }
    auto interval = cJSON_GetObjectItem(manifest, "intervalSec");
    auto loop = cJSON_GetObjectItem(manifest, "loop");
    if (!SaveManifest(resource->valuestring, version->valuestring,
                      cJSON_IsNumber(interval) ? interval->valueint : 5,
                      !cJSON_IsBool(loop) || cJSON_IsTrue(loop), next)) {
        ESP_LOGE(TAG, "Gallery manifest commit failed");
        return rollback();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_ = next;
        resource_id_ = resource->valuestring;
        content_version_ = version->valuestring;
        interval_sec_ = cJSON_IsNumber(interval) ? std::max(3, std::min(interval->valueint, 60)) : 5;
        loop_ = !cJSON_IsBool(loop) || cJSON_IsTrue(loop);
    }
    for (const auto& path : superseded) std::remove(path.c_str());
    ESP_LOGI(TAG, "Gallery appended: resource=%s added=%d total=%u bytes=%u", resource->valuestring,
             count, static_cast<unsigned>(next.size()), static_cast<unsigned>(total_bytes));
    return true;
}

bool GalleryStore::SaveManifest(const std::string& resource_id, const std::string& version,
                                int interval_sec, bool loop, const std::vector<Item>& items) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "resourceId", resource_id.c_str());
    cJSON_AddStringToObject(root, "contentVersion", version.c_str());
    cJSON_AddNumberToObject(root, "intervalSec", std::max(3, std::min(interval_sec, 60)));
    cJSON_AddBoolToObject(root, "loop", loop);
    cJSON* files = cJSON_AddArrayToObject(root, "files");
    for (const auto& item : items) {
        cJSON* row = cJSON_CreateObject();
        cJSON_AddStringToObject(row, "itemId", item.item_id.c_str());
        const auto slash = item.path.find_last_of('/');
        cJSON_AddStringToObject(row, "fileName", item.path.substr(slash + 1).c_str());
        cJSON_AddStringToObject(row, "format", item.format.c_str());
        cJSON_AddStringToObject(row, "sha256", item.sha256.c_str());
        cJSON_AddNumberToObject(row, "sizeBytes", static_cast<double>(item.bytes));
        cJSON_AddItemToArray(files, row);
    }
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return false;
    const std::string path = ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, "manifest.jsn");
    const std::string next = ContentStorage::GetInstance().GetPath(ContentStorage::Category::Gallery, "manifest.tmp");
    FILE* file = fopen(next.c_str(), "wb");
    const size_t length = strlen(json);
    const bool ok = file && fwrite(json, 1, length, file) == length && fflush(file) == 0;
    if (file) fclose(file);
    cJSON_free(json);
    if (!ok) return false;
    std::remove(path.c_str());
    return std::rename(next.c_str(), path.c_str()) == 0;
}

bool GalleryStore::DeleteItem(const std::string& item_id) {
    std::vector<Item> next;
    Item removed;
    std::string resource;
    std::string version;
    int interval;
    bool loop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& item) { return item.item_id == item_id; });
        if (found == items_.end()) return true;
        removed = *found;
        next = items_;
        next.erase(next.begin() + std::distance(items_.begin(), found));
        resource = resource_id_;
        version = content_version_;
        interval = interval_sec_;
        loop = loop_;
    }
    if (!SaveManifest(resource, version, interval, loop, next)) return false;
    std::remove(removed.path.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    items_ = std::move(next);
    return true;
}

size_t GalleryStore::Count() const { std::lock_guard<std::mutex> lock(mutex_); return items_.size(); }
GalleryStore::Item GalleryStore::ItemAt(size_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < items_.size() ? items_[index] : Item{};
}
std::vector<GalleryStore::Item> GalleryStore::Items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}
std::string GalleryStore::ResourceId() const { std::lock_guard<std::mutex> lock(mutex_); return resource_id_; }
std::string GalleryStore::ContentVersion() const { std::lock_guard<std::mutex> lock(mutex_); return content_version_; }
int GalleryStore::IntervalSec() const { std::lock_guard<std::mutex> lock(mutex_); return interval_sec_; }
bool GalleryStore::Loop() const { std::lock_guard<std::mutex> lock(mutex_); return loop_; }
