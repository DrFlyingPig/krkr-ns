#pragma once

#include <unordered_set>
#include <vector>

namespace emoteplayer {

template <typename Motion>
bool IsFiniteMotion(Motion* motion)
{
    if (motion->loopTime >= 0 || motion->lastTime < 0 || motion->isParameterize)
        return false;
    for (auto* node : motion->nodeList)
        if (node && node->isParameterize) return false;
    return true;
}

// A finite parent does not imply that a referenced looping/parameterized
// motion has finished.  Check the immutable graph once when playback starts.
template <typename Motion, typename IsFinite, typename Children>
bool IsFinitePlaybackGraph(Motion* root, IsFinite isFinite, Children children)
{
    if (!root) return false;
    struct Entry { Motion* motion; bool leaving; };
    std::vector<Entry> pending{{root, false}};
    std::unordered_set<Motion*> visiting, visited;
    while (!pending.empty()) {
        const Entry entry = pending.back();
        pending.pop_back();
        if (entry.leaving) {
            visiting.erase(entry.motion);
            visited.insert(entry.motion);
            continue;
        }
        if (visited.count(entry.motion)) continue;
        if (!visiting.insert(entry.motion).second || !isFinite(entry.motion))
            return false;
        pending.push_back({entry.motion, true});
        for (Motion* child : children(entry.motion)) {
            if (!child) return false; // An unresolved motion is not proven finite.
            pending.push_back({child, false});
        }
    }
    return true;
}

inline bool HasCompletedFinitePlayback(bool playing, bool finiteGraph,
                                      bool parameterized, bool hasTimelines)
{
    return !playing && finiteGraph && !parameterized && !hasTimelines;
}

} // namespace emoteplayer
