#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace nr {

// Descriptor contents belong to a recording until Reset/destruction, including
// after submission: a closed command list may execute again. Discarding that
// recording does not retire its executions on any of the independent queues.
class DescriptorLifetime {
public:
    std::uint64_t owner = 0;

    void assign(std::uint64_t recording) {
        owner = recording;
        waits_.clear(); // caller must first establish reusable()
    }
    void discard(std::uint64_t recording) {
        if (owner == recording) owner = 0;
    }
    void submit(std::uint64_t recording, std::uint64_t queue, std::uint64_t value) {
        if (!owner || owner != recording) return;
        for (auto &wait : waits_) {
            if (wait.queue == queue) {
                if (value > wait.value) wait.value = value;
                return;
            }
        }
        waits_.push_back({queue, value});
    }
    template <typename Completed> bool reusable(Completed completed) const {
        if (owner) return false;
        for (const auto &wait : waits_) {
            const auto value = completed(wait.queue);
            if (value == std::numeric_limits<std::uint64_t>::max() || value < wait.value)
                return false;
        }
        return true;
    }

private:
    struct Wait { std::uint64_t queue, value; };
    std::vector<Wait> waits_;
};

} // namespace nr
