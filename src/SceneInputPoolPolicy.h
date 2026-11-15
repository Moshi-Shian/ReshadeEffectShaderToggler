#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Rendering {
constexpr uint64_t SceneMiB = 1024ull * 1024;
constexpr size_t SceneSplitImageLimit = 32; // Changed from 8

struct SceneMemoryBudget {
    uint64_t budget = 0, usage = 0;
    bool known = false;
};
inline uint64_t ScenePoolByteLimit(bool split, uint64_t imageBytes, const SceneMemoryBudget& memory) {
    // Growth is specific to the opt-in full-precision compositor split. Never
    // infer available memory from the card's advertised/dedicated capacity.
    uint64_t limit = (split ? 512ull : 256ull) * SceneMiB;
    if (split && memory.known) {
        const auto target = std::min(imageBytes, 1536 * SceneMiB / SceneSplitImageLimit) * SceneSplitImageLimit;
        limit = std::min(1536 * SceneMiB, std::max(limit, target));
    }
    return memory.known ? std::min(limit, memory.budget / 4) : limit;
}
inline const char* SceneAllocationRejection(bool split, size_t images, uint64_t used, uint64_t requested,
                                           uint64_t limit, const SceneMemoryBudget& memory) {
    if (split && images >= SceneSplitImageLimit)
        return "Scene input: active image capacity reached";
    if (requested > limit || used > limit - requested)
        return "Scene input: private image byte limit reached";
    if (memory.known && (memory.usage >= memory.budget || requested > (memory.budget - memory.usage) / 2))
        return "Scene input: insufficient GPU budget headroom";
    return nullptr;
}

enum class SceneImageAvailability { available, incompatible, unsafe, active };
inline SceneImageAvailability ClassifySceneImage(bool compatible, bool reusable, bool owned, bool sameRecording) {
    if (!compatible) return SceneImageAvailability::incompatible;
    if (!reusable) return SceneImageAvailability::unsafe;
    return owned && !sameRecording ? SceneImageAvailability::active : SceneImageAvailability::available;
}
}
