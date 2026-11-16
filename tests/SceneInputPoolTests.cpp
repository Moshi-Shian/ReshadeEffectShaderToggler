#include "SceneInputPoolPolicy.h"
#include "SceneInputSync.h"
#include <array>
#include <cstdio>
#include <stdexcept>

using namespace Rendering;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        const uint64_t pair = 3818ull * 1600 * 20; // Reported resolution's texel payload; production queries native heap sizes.
        const SceneMemoryBudget roomy{8ull * 1024 * SceneMiB, 6ull * 1024 * SceneMiB, true};
        const auto limit = ScenePoolByteLimit(true, pair, roomy);
        Check(limit >= pair * 32, "Reported resolution cannot support eight active images");
        std::array<SceneInputOwnership, 8> images;
        std::array<int, 9> recordings{};
        uint64_t used = 0;
        for (size_t i = 0; i < images.size(); ++i) {
            for (size_t j = 0; j < i; ++j)
                Check(!images[j].CanAcquire(&recordings[i]), "Another active recording's image was shared");
            Check(!SceneAllocationRejection(true, i, used, pair, limit, roomy), "Allocation rejected before eighth recording");
            images[i].Acquire(&recordings[i], &recordings[i]);
            used += pair;
        }
        Check(SceneAllocationRejection(true, 32, used, pair, limit, roomy) != nullptr, "33th active image exceeds count bound");
        for (const auto& image : images)
            Check(!image.CanAcquire(&recordings[8]), "Ninth recording stole a live image");
        images[3].Retire(&recordings[3], true, {});
        Check(images[3].CanAcquire(&recordings[8]), "Retired image cannot satisfy ninth recording");
        images[3].Acquire(&recordings[8], &recordings[8]);
        Check(!images[3].CanAcquire(&recordings[3]), "Old owner can take image back");

        Check(ScenePoolByteLimit(true, pair, {}) == 512 * SceneMiB, "Unknown budget enabled unchecked growth");
        Check(SceneAllocationRejection(true, 4, 4 * pair, pair, 512 * SceneMiB, {}) != nullptr,
              "Baseline test does not reproduce Preview 5 exhaustion");
        Check(ScenePoolByteLimit(false, pair, roomy) == 256 * SceneMiB, "Regular scene budget changed");
        Check(ScenePoolByteLimit(true, UINT64_MAX, roomy) == 4096 * SceneMiB, "Large target overflowed the hard cap");
        const SceneMemoryBudget smallBudget{1024 * SceneMiB, 0, true};
        Check(ScenePoolByteLimit(true, pair, smallBudget) == 512 * SceneMiB, "Half-budget cap ignored");
        const SceneMemoryBudget tight{8ull * 1024 * SceneMiB, 8ull * 1024 * SceneMiB - pair, true};
        Check(SceneAllocationRejection(true, 4, 4 * pair, pair, limit, tight) != nullptr, "Half-headroom reserve ignored");
        const SceneMemoryBudget over{1024 * SceneMiB, 1024 * SceneMiB + 1, true};
        Check(SceneAllocationRejection(true, 0, 0, 1, limit, over) != nullptr, "Over-budget usage underflowed");
        Check(SceneAllocationRejection(false, 0, UINT64_MAX, pair, limit, roomy) != nullptr, "Pool byte overflow accepted");

        Check(ClassifySceneImage(true, true, false, false) == SceneImageAvailability::available, "Idle compatible image unavailable");
        Check(ClassifySceneImage(true, true, true, true) == SceneImageAvailability::available, "Sequential reuse blocked");
        Check(ClassifySceneImage(true, true, true, false) == SceneImageAvailability::active, "Active ownership misclassified");
        Check(ClassifySceneImage(false, true, false, false) == SceneImageAvailability::incompatible, "Old format/size image misclassified");
        Check(ClassifySceneImage(true, false, false, false) == SceneImageAvailability::unsafe, "Failed retirement treated as available");
        std::puts("Eight simultaneously active recordings, Preview 5 exhaustion, safe retirement, memory bounds and image classification passed.");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}

