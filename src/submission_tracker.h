#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace nr {

// Keys are stable private-data tokens shared by a command list's native and
// wrapper interfaces, not interface pointer values. The owner serializes calls.
// This tracks recordings only: submission fences must outlive reset/discard.
class SubmissionTracker {
public:
    void record(std::uint64_t id) {
        for (auto &entry : entries_) {
            if (entry.id == id) {
                entry.submitted = false;
                return;
            }
        }
        entries_.push_back({id, false});
    }

    // A closed recording may execute repeatedly; each known submission needs
    // another queue fence even if an earlier submission has already completed.
    bool submit(std::uint64_t id) {
        for (auto &entry : entries_) {
            if (entry.id == id) {
                entry.submitted = true;
                return true;
            }
        }
        return false;
    }

    // Reset or destruction discards the current recording. Already submitted
    // GPU work remains protected by the owner's separate queue fences.
    void discard(std::uint64_t id) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
            [id](const Entry &entry) { return entry.id == id; }), entries_.end());
    }

    bool ready() const { return pending_count() == 0; }
    std::size_t size() const { return entries_.size(); }
    std::size_t pending_count() const {
        return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(),
            [](const Entry &entry) { return !entry.submitted; }));
    }
    void clear() { entries_.clear(); }

private:
    struct Entry { std::uint64_t id; bool submitted; };
    std::vector<Entry> entries_;
};

} // namespace nr
