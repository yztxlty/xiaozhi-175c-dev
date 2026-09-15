#pragma once

#include <ctime>

struct WatchHandTurns {
    double hours;
    double minutes;
    double seconds;
};

inline WatchHandTurns ComputeWatchHandTurns(const std::tm& local) {
    const double seconds = local.tm_sec / 60.0;
    const double minutes = (local.tm_min + seconds) / 60.0;
    return {((local.tm_hour % 12) + minutes) / 12.0, minutes, seconds};
}
