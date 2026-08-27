#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class ContentStorage {
public:
    enum class Health {
        Ready,
        Unformatted,
        Corrupt,
        Unavailable,
    };

    enum class Category {
        Music,
        Games,
        SmallFiles,
    };

    struct Stats {
        size_t total_bytes;
        size_t free_bytes;
        size_t music_bytes;
        size_t game_bytes;
        size_t small_bytes;
    };

    static constexpr size_t kMusicQuotaBytes = 8 * 1024 * 1024;
    static constexpr size_t kGameQuotaBytes = 2 * 1024 * 1024;
    static constexpr size_t kSmallFilesQuotaBytes = 512 * 1024;
    static constexpr size_t kSafetyReserveBytes = 1536 * 1024;

    static ContentStorage& GetInstance();
    ~ContentStorage();

    bool Initialize();
    Stats GetStats() const;
    bool CanReserve(Category category, size_t bytes) const;
    std::string GetPath(Category category, const std::string& file_name) const;
    Health GetHealth() const { return health_; }

    static bool IsSafeFileName(const std::string& file_name) {
        if (file_name.empty() || file_name.size() > 128 || file_name.find("..") != std::string::npos) {
            return false;
        }
        constexpr const char* kForbidden = "/\\:*?\"<>|";
        for (unsigned char character : file_name) {
            if (character < 0x20 || std::string(kForbidden).find(static_cast<char>(character)) != std::string::npos) {
                return false;
            }
        }
        return true;
    }

    static bool CanReserve(const Stats& stats, Category category, size_t bytes) {
        size_t used = 0;
        size_t quota = 0;
        switch (category) {
            case Category::Music:
                used = stats.music_bytes;
                quota = kMusicQuotaBytes;
                break;
            case Category::Games:
                used = stats.game_bytes;
                quota = kGameQuotaBytes;
                break;
            case Category::SmallFiles:
                used = stats.small_bytes;
                quota = kSmallFilesQuotaBytes;
                break;
        }
        if (used > quota || bytes > quota - used) {
            return false;
        }
        return stats.free_bytes > kSafetyReserveBytes &&
               bytes <= stats.free_bytes - kSafetyReserveBytes;
    }

    static std::string BuildPath(Category category, const std::string& file_name) {
        if (!IsSafeFileName(file_name)) {
            return {};
        }
        switch (category) {
            case Category::Music:
                return "/content/music/" + file_name;
            case Category::Games:
                return "/content/games/" + file_name;
            case Category::SmallFiles:
                return "/content/save/" + file_name;
        }
        return {};
    }

private:
    ContentStorage() = default;
    ContentStorage(const ContentStorage&) = delete;
    ContentStorage& operator=(const ContentStorage&) = delete;

    Health health_ = Health::Unavailable;
    int32_t wl_handle_ = -1;
};
