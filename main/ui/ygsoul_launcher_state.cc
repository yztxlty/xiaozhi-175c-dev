#include "ygsoul_launcher_state.h"

void YGSoulLauncherState::Gesture(YGSoulGesture gesture) {
    if (page_ == YGSoulPage::kFeatures) {
        if (gesture == YGSoulGesture::kLeft) feature_page_ = 1;
        else if (gesture == YGSoulGesture::kRight) feature_page_ = 0;
        else if (gesture == YGSoulGesture::kDown) Home();
        return;
    }
    if (page_ == YGSoulPage::kMusicPlaylist && music_count_) {
        if (gesture == YGSoulGesture::kUp) music_index_ = (music_index_ + 1) % music_count_;
        else if (gesture == YGSoulGesture::kDown) music_index_ = (music_index_ + music_count_ - 1) % music_count_;
        return;
    }
    if (page_ == YGSoulPage::kGallery && gallery_count_) {
        if (gesture == YGSoulGesture::kLeft) gallery_index_ = (gallery_index_ + 1) % gallery_count_;
        else if (gesture == YGSoulGesture::kRight) gallery_index_ = (gallery_index_ + gallery_count_ - 1) % gallery_count_;
        else if (gesture == YGSoulGesture::kDown) page_ = YGSoulPage::kDesktop;
        return;
    }
    if (page_ == YGSoulPage::kSettings && gesture == YGSoulGesture::kUp) page_ = YGSoulPage::kDesktop;
    else if (page_ == YGSoulPage::kDesktop && gesture == YGSoulGesture::kUp) page_ = YGSoulPage::kFeatures;
    else if (page_ == YGSoulPage::kDesktop && gesture == YGSoulGesture::kDown) page_ = YGSoulPage::kSettings;
    else if (page_ != YGSoulPage::kDesktop && gesture == YGSoulGesture::kDown) page_ = YGSoulPage::kDesktop;
}

void YGSoulLauncherState::Back() {
    if (page_ == YGSoulPage::kMusicPlayer) page_ = YGSoulPage::kMusicPlaylist;
    else if (page_ == YGSoulPage::kMusicPlaylist || page_ == YGSoulPage::kGallery ||
             page_ == YGSoulPage::kSettings || page_ == YGSoulPage::kWatch) page_ = YGSoulPage::kFeatures;
    else page_ = YGSoulPage::kDesktop;
}

void YGSoulLauncherState::OpenFeature(size_t index) {
    static constexpr YGSoulPage pages[] = {
        YGSoulPage::kMusicPlaylist, YGSoulPage::kGallery, YGSoulPage::kChat,
        YGSoulPage::kSettings, YGSoulPage::kWatch,
    };
    if (index < sizeof(pages) / sizeof(pages[0])) page_ = pages[index];
}
