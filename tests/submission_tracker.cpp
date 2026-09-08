#include "submission_tracker.h"

#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void stable_identity_across_interfaces() {
    nr::SubmissionTracker tracker;
    // The D3D12 layer supplies the same private-data token for both views.
    const std::uint64_t wrapper_token = 42, native_token = 42;
    tracker.record(wrapper_token);
    require(tracker.pending_count() == 1, "recording starts pending");
    require(tracker.submit(native_token), "native submission matches wrapper recording");
    require(tracker.ready(), "matched alias clears pending recording");
    require(!tracker.submit(100), "unrelated list is not a tracked submission");
    require(tracker.size() == 1, "unknown submission does not grow tracker");
}

static void nine_recordings_at_resize() {
    nr::SubmissionTracker tracker;
    require(tracker.ready(), "empty state can clean up");
    for (std::uint64_t id = 1; id <= 9; ++id) tracker.record(id);
    require(tracker.size() == 9 && tracker.pending_count() == 9,
            "reproduce AC's nine unmatched recordings");

    for (std::uint64_t id = 1; id <= 5; ++id)
        require(tracker.submit(id), "match submitted AC recordings");
    tracker.discard(6); // Reset without submitting.
    tracker.discard(7); // Reset without submitting.
    tracker.discard(8); // List destroyed without submitting.
    require(!tracker.ready() && tracker.pending_count() == 1,
            "one live unsubmitted recording still blocks cleanup");
    tracker.discard(9);
    require(tracker.ready() && tracker.size() == 5,
            "submitted or discarded recordings no longer strand resize");
    tracker.clear();
    require(tracker.ready() && tracker.size() == 0, "cleanup resets tracker");
}

static void rerecord_and_repeat_submission() {
    nr::SubmissionTracker tracker;
    tracker.record(7);
    require(tracker.submit(7), "initial submission is tracked");
    require(tracker.submit(7), "repeat submission still requires a new fence");
    tracker.record(7);
    require(tracker.size() == 1 && tracker.pending_count() == 1 && !tracker.ready(),
            "re-recording invalidates prior submission readiness");
    require(tracker.submit(7) && tracker.ready(), "new recording has its own submission");
    tracker.discard(7);
    require(!tracker.submit(7), "reset recording is no longer a known submission");
    tracker.record(7);
    require(!tracker.ready(), "reused identity starts a fresh pending recording");
}

static void discard_preserves_external_fence_requirement() {
    nr::SubmissionTracker tracker;
    std::uint64_t fence_value = 0, completed_value = 0;
    auto submit = [&](std::uint64_t id) {
        if (tracker.submit(id)) ++fence_value;
    };
    auto can_release = [&] { return tracker.ready() && completed_value >= fence_value; };

    tracker.record(3);
    submit(3);
    completed_value = fence_value;
    require(can_release(), "completed submission permits release");
    submit(3); // The game executes a closed list again.
    tracker.discard(3); // Reset after submitting does not cancel queued work.
    require(tracker.ready() && !can_release(),
            "discard clears recording but latest queue fence still blocks release");
    completed_value = fence_value;
    require(can_release(), "last submission completion permits release after reset");
}

int main() {
    stable_identity_across_interfaces();
    nine_recordings_at_resize();
    rerecord_and_repeat_submission();
    discard_preserves_external_fence_requirement();
    std::puts("submission tracker: all regression tests passed");
}
