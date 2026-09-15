#pragma once

#include <cstddef>
#include <string>

struct cJSON;

class WatchFaceStore {
public:
    static WatchFaceStore& GetInstance();

    bool Apply(const cJSON* params);
    std::string CurrentPath() const;
    std::string ResourceId() const;
    std::string ContentVersion() const;

private:
    WatchFaceStore() = default;
    std::string SlotPath(const std::string& slot) const;
};
