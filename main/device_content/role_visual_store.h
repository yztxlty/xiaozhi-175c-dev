#pragma once

#include <cstddef>
#include <string>

class RoleVisualStore {
public:
    static RoleVisualStore& GetInstance();

    bool Prepare(const std::string& resource_id, int version,
                 const std::string& url, const std::string& sha256,
                 size_t expected_bytes);
    bool Commit(const std::string& role_id, const std::string& resource_id,
                int resource_version, int configuration_revision);
    void LoadActive();
    void Clear();

private:
    RoleVisualStore() = default;
    std::string SlotPath(const std::string& slot) const;
};
