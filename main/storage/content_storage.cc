#include "content_storage.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>

#define TAG "ContentStorage"

namespace {

constexpr const char* kMountPoint = "/content";
constexpr const char* kPartitionLabel = "content";
constexpr std::array<const char*, 5> kContentDirectories = {
    "/content/music",
    "/content/games",
    "/content/covers",
    "/content/save",
    "/content/tmp",
};

size_t DirectoryBytes(const char* path) {
    DIR* directory = opendir(path);
    if (directory == nullptr) {
        return 0;
    }
    size_t total = 0;
    while (dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        std::string child = std::string(path) + "/" + entry->d_name;
        struct stat info {};
        if (stat(child.c_str(), &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            total += DirectoryBytes(child.c_str());
        } else if (S_ISREG(info.st_mode)) {
            total += static_cast<size_t>(info.st_size);
        }
    }
    closedir(directory);
    return total;
}

bool EnsureDirectories() {
    for (const char* path : kContentDirectories) {
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create %s: %s", path, strerror(errno));
            return false;
        }
    }
    return true;
}

}  // namespace

ContentStorage& ContentStorage::GetInstance() {
    static ContentStorage instance;
    return instance;
}

ContentStorage::~ContentStorage() {
    if (wl_handle_ >= 0) {
        esp_vfs_fat_spiflash_unmount_rw_wl(kMountPoint, wl_handle_);
        wl_handle_ = -1;
    }
}

bool ContentStorage::Initialize() {
    if (health_ == Health::Ready) {
        return true;
    }

    esp_vfs_fat_mount_config_t mount_config {};
    // The migration flashes a pre-formatted wear-levelled FAT image. Never
    // format at runtime: a failed mount must preserve every recoverable byte.
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 12;
    mount_config.allocation_unit_size = 4096;
    mount_config.disk_status_check_enable = false;
    mount_config.use_one_fat = false;
    esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(
        kMountPoint, kPartitionLabel, &mount_config, &wl_handle_);
    if (result != ESP_OK) {
        wl_handle_ = -1;
        health_ = result == ESP_ERR_NOT_FOUND ? Health::Unavailable : Health::Corrupt;
        ESP_LOGE(TAG, "Failed to mount content partition without data loss: %s", esp_err_to_name(result));
        return false;
    }

    if (!EnsureDirectories()) {
        esp_vfs_fat_spiflash_unmount_rw_wl(kMountPoint, wl_handle_);
        wl_handle_ = -1;
        health_ = Health::Corrupt;
        return false;
    }

    health_ = Health::Ready;
    const Stats stats = GetStats();
    ESP_LOGI(TAG, "Content storage ready: total=%u KiB free=%u KiB",
             static_cast<unsigned>(stats.total_bytes / 1024),
             static_cast<unsigned>(stats.free_bytes / 1024));
    return true;
}

ContentStorage::Stats ContentStorage::GetStats() const {
    Stats stats {};
    if (health_ != Health::Ready) {
        return stats;
    }
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(kMountPoint, &total_bytes, &free_bytes) == ESP_OK) {
        stats.total_bytes = static_cast<size_t>(total_bytes);
        stats.free_bytes = static_cast<size_t>(free_bytes);
    }
    stats.music_bytes = DirectoryBytes("/content/music");
    stats.game_bytes = DirectoryBytes("/content/games");
    stats.small_bytes = DirectoryBytes("/content/covers") + DirectoryBytes("/content/save");
    return stats;
}

bool ContentStorage::CanReserve(Category category, size_t bytes) const {
    return health_ == Health::Ready && CanReserve(GetStats(), category, bytes);
}

std::string ContentStorage::GetPath(Category category, const std::string& file_name) const {
    if (health_ != Health::Ready) {
        return {};
    }
    return BuildPath(category, file_name);
}
