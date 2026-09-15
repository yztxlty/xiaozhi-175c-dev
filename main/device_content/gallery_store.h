#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

struct cJSON;

class GalleryStore {
public:
    struct Item {
        std::string item_id;
        std::string path;
        std::string format;
        std::string sha256;
        size_t bytes = 0;
    };

    static GalleryStore& GetInstance();

    bool Load();
    bool Apply(const cJSON* params);
    bool DeleteItem(const std::string& item_id);
    size_t Count() const;
    Item ItemAt(size_t index) const;
    std::vector<Item> Items() const;
    std::string ResourceId() const;
    std::string ContentVersion() const;
    int IntervalSec() const;
    bool Loop() const;

private:
    GalleryStore() = default;
    bool SaveManifest(const std::string& resource_id, const std::string& version,
                      int interval_sec, bool loop, const std::vector<Item>& items);
    static bool SafeId(const std::string& value);

    mutable std::mutex mutex_;
    std::string resource_id_;
    std::string content_version_;
    int interval_sec_ = 5;
    bool loop_ = true;
    std::vector<Item> items_;
};
