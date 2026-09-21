#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct cJSON;

class RoleAnimationStore {
public:
    struct Item {
        std::string action_code;
        std::string asset_id;
        int asset_version = 0;
        std::string path;
        std::string sha256;
        size_t bytes = 0;
    };

    static RoleAnimationStore& GetInstance();

    bool Load();
    bool Apply(const cJSON* params, const std::string& request_id);
    bool Show(const std::string& action_code);
    void Restore();
    void ClearForRoleChange(const std::string& role_id);
    void AddReported(cJSON* reported, const std::string& request_id = "") const;

private:
    RoleAnimationStore() = default;
    bool SaveManifest(const std::string& slot, const cJSON* manifest,
                      const std::vector<Item>& items) const;
    std::string ManifestPath(const std::string& slot) const;

    std::string role_id_;
    std::string manifest_sha256_;
    std::string last_request_id_;
    int configuration_revision_ = 0;
    int revision_ = 0;
    std::string current_action_;
    std::vector<Item> items_;
};
