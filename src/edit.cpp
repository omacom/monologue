#include "edit.h"
#include <algorithm>

edit::Clips edit::whole(double duration) { return {{0, std::max(0.0, duration)}}; }

edit::Clips edit::normalized(Clips clips, double duration) {
    duration = std::max(0.0, duration);
    std::sort(clips.begin(), clips.end(), [](const Range &a, const Range &b) { return a.start < b.start; });
    Clips result;
    for (auto clip : clips) {
        clip.start = std::clamp(clip.start, 0.0, duration);
        clip.end = std::clamp(clip.end, 0.0, duration);
        if (clip.start < minimumGap) clip.start = 0;
        if (duration - clip.end < minimumGap) clip.end = duration;
        if (!result.isEmpty() && clip.start - result.last().end < minimumGap) clip.start = result.last().end;
        if (clip.length() >= minimumClip) result.append(clip);
    }
    return result;
}

QList<edit::Range> edit::kept(const Clips &clips) {
    QList<Range> result;
    for (const auto &clip : clips) {
        if (!result.isEmpty() && clip.start <= result.last().end) result.last().end = std::max(result.last().end, clip.end);
        else result.append(clip);
    }
    return result;
}

double edit::keptDuration(const Clips &clips) {
    double total = 0;
    for (const auto &range : kept(clips)) total += range.length();
    return total;
}

bool edit::untouched(const Clips &clips, double duration) {
    const auto ranges = kept(clips);
    return ranges.size() == 1 && ranges.first().start <= 0 && ranges.first().end >= duration;
}

