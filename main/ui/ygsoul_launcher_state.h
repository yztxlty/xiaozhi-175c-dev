#pragma once

#include <cstddef>

enum class YGSoulPage {
    kDesktop, kFeatures, kMusicPlaylist, kMusic = kMusicPlaylist,
    kMusicPlayer, kGallery, kChat, kSettings, kWatch,
};
enum class YGSoulGesture { kUp, kDown, kLeft, kRight };

class YGSoulLauncherState {
public:
    explicit YGSoulLauncherState(size_t gallery_count = 0, size_t music_count = 0)
        : gallery_count_(gallery_count), music_count_(music_count) {}
    void Gesture(YGSoulGesture gesture);
    void OpenFeature(size_t index);
    void OpenMusicPlayer() { page_ = YGSoulPage::kMusicPlayer; }
    void SetMusicCount(size_t count) {
        music_count_ = count;
        if (music_index_ >= music_count_) music_index_ = 0;
    }
    void SetMusicIndex(size_t index) { if (index < music_count_) music_index_ = index; }
    void SetGalleryCount(size_t count) {
        gallery_count_ = count;
        if (gallery_index_ >= gallery_count_) gallery_index_ = 0;
    }
    void Back();
    void Home() { page_ = YGSoulPage::kDesktop; feature_page_ = 0; }
    YGSoulPage page() const { return page_; }
    size_t feature_page() const { return feature_page_; }
    size_t gallery_index() const { return gallery_index_; }
    size_t music_index() const { return music_index_; }

private:
    YGSoulPage page_ = YGSoulPage::kDesktop;
    size_t feature_page_ = 0;
    size_t gallery_count_ = 0;
    size_t gallery_index_ = 0;
    size_t music_count_ = 0;
    size_t music_index_ = 0;
};
