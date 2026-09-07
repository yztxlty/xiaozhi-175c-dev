#ifndef ASSETS_H
#define ASSETS_H

#include <string>
#include <functional>
#include <memory>

#include <cJSON.h>
#include <esp_partition.h>
#include <model_path.h>
#include <map>
#include <string>

#if HAVE_LVGL
#include <spi_flash_mmap.h>
#endif

struct Asset {
    size_t size;
    size_t offset;
};

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }
    ~Assets();

    bool Download(std::string url, std::function<void(int progress, size_t speed)> progress_callback);
    bool Apply();
    bool GetAssetData(const std::string& name, void*& ptr, size_t& size);

    inline bool partition_valid() const { return partition_valid_; }
    inline std::string default_assets_url() const { return default_assets_url_; }
    size_t GetFreeSpace() const {
        if (!partition_valid_ || !partition_) {
            return 0;
        }
        const size_t used = used_size_ + transient_size_;
        return used <= partition_->size ? partition_->size - used : 0;
    }
    size_t GetTotalSize() const {
        return partition_valid_ && partition_ ? partition_->size : 0;
    }
    size_t GetUsedSize() const {
        return partition_valid_ ? used_size_ + transient_size_ : 0;
    }
    bool PurgeTransient();

private:
    Assets();
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;

    bool InitializePartition();
    void UnApplyPartition();
    size_t ScanTransientOccupancy(size_t keep) const;
    static size_t AlignUp(size_t value, size_t align, size_t cap);
    static bool FindPartition(Assets* assets);
    static bool LoadSrmodelsFromIndex(Assets* assets, cJSON* root = nullptr);
  
    class AssetStrategy {
    public:
        virtual ~AssetStrategy() = default;
        virtual bool Apply(Assets* assets) = 0;
        virtual bool InitializePartition(Assets* assets) = 0;
        virtual void UnApplyPartition(Assets* assets) = 0;
        virtual bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) = 0;
    };
    
    class LvglStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    private:
        static uint32_t CalculateChecksum(const char* data, uint32_t length);
        std::map<std::string, Asset> assets_;
        esp_partition_mmap_handle_t mmap_handle_ = 0;
        const char* mmap_root_ = nullptr;
        bool checksum_valid_ = false;
    };
    
    class EmoteStrategy : public AssetStrategy {
    public:
        bool Apply(Assets* assets) override;
        bool InitializePartition(Assets* assets) override;
        void UnApplyPartition(Assets* assets) override;
        bool GetAssetData(Assets* assets, const std::string& name, void*& ptr, size_t& size) override;
    };
    
    // Strategy instance
    std::unique_ptr<AssetStrategy> strategy_;

protected:
    const esp_partition_t* partition_ = nullptr;
    bool partition_valid_ = false;
    bool pack_layout_known_ = false;
    bool pack_valid_ = false;
    size_t used_size_ = 0;
    size_t transient_size_ = 0;
    std::string default_assets_url_;
    srmodel_list_t* models_list_ = nullptr;
};

#endif
