#include "descriptor_lifetime.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    std::array<std::uint64_t, 3> done{};
    auto completed = [&](std::uint64_t queue) { return done.at(queue); };
    nr::DescriptorLifetime page;
    require(page.reusable(completed), "new page is reusable");
    page.assign(42);
    require(!page.reusable(completed), "unsubmitted recording protects descriptors");
    page.submit(42, 0, 1);
    done[0] = 1;
    require(!page.reusable(completed), "completed closed list can execute again");
    page.submit(42, 0, 2);
    page.discard(42);
    require(!page.reusable(completed), "Reset retains the repeated execution wait");
    done[0] = 2;
    require(page.reusable(completed), "Reset plus completion permits reuse");

    page.assign(43);
    page.submit(43, 0, 3);
    page.submit(43, 1, 100);
    page.submit(43, 2, 5);
    page.discard(43);
    done[1] = 100;
    done[2] = 5;
    require(!page.reusable(completed), "larger value on another queue proves nothing");
    done[0] = 3;
    require(page.reusable(completed), "all three queue timelines completed");
    done[0] = std::numeric_limits<std::uint64_t>::max();
    require(!page.reusable(completed), "device removal is not completion");
    done[0] = 3;

    page.assign(44);
    page.discard(99);
    require(!page.reusable(completed), "unrelated Reset cannot release ownership");
    page.submit(99, 0, 900);
    page.discard(44);
    require(page.reusable(completed), "abandoned unsubmitted recording can be reused");

    // The old fixed 16-entry bookkeeping silently lost later recordings.
    std::array<nr::DescriptorLifetime, 80> live;
    for (std::size_t i = 0; i < live.size(); ++i) {
        live[i].assign(i + 1);
        live[i].submit(i + 1, 0, 10 + i);
    }
    done[0] = 88;
    for (std::size_t i = 0; i < live.size(); ++i) {
        require(!live[i].reusable(completed), "live recording must keep its descriptors");
        live[i].discard(i + 1);
        require(live[i].reusable(completed) == (i < 79), "each page retains its own wait");
    }
    done[0] = 89;
    require(live.back().reusable(completed), "last recording is tracked beyond 16 entries");
    std::puts("descriptor lifetime: all regression tests passed");
}
