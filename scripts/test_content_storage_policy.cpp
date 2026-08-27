#include "main/storage/content_storage.h"

#include <cassert>
#include <string>

int main() {
    using Category = ContentStorage::Category;

    assert(ContentStorage::IsSafeFileName("track-01.opus"));
    assert(ContentStorage::IsSafeFileName("幽光音乐.opus"));
    assert(!ContentStorage::IsSafeFileName(""));
    assert(!ContentStorage::IsSafeFileName("../track.opus"));
    assert(!ContentStorage::IsSafeFileName("folder/track.opus"));
    assert(!ContentStorage::IsSafeFileName("folder\\track.opus"));

    ContentStorage::Stats stats{
        12 * 1024 * 1024,
        4 * 1024 * 1024,
        7 * 1024 * 1024,
        1 * 1024 * 1024,
        256 * 1024,
    };
    assert(ContentStorage::CanReserve(stats, Category::Music, 1024 * 1024));
    assert(!ContentStorage::CanReserve(stats, Category::Music, 1024 * 1024 + 1));
    assert(ContentStorage::CanReserve(stats, Category::Games, 1024 * 1024));
    assert(!ContentStorage::CanReserve(stats, Category::Games, 1024 * 1024 + 1));
    assert(ContentStorage::CanReserve(stats, Category::SmallFiles, 256 * 1024));
    assert(!ContentStorage::CanReserve(stats, Category::SmallFiles, 256 * 1024 + 1));

    stats.free_bytes = ContentStorage::kSafetyReserveBytes;
    assert(!ContentStorage::CanReserve(stats, Category::Music, 1));
    stats.free_bytes = ContentStorage::kSafetyReserveBytes + 1;
    assert(ContentStorage::CanReserve(stats, Category::Music, 1));

    assert(ContentStorage::BuildPath(Category::Music, "track.opus") == "/content/music/track.opus");
    assert(ContentStorage::BuildPath(Category::Games, "level.pack") == "/content/games/level.pack");
    assert(ContentStorage::BuildPath(Category::SmallFiles, "save.dat") == "/content/save/save.dat");
    assert(ContentStorage::BuildPath(Category::Music, "../bad") == "");
    return 0;
}
