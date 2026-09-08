#include "feature_registry.h"

#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() {
    nr::FeatureRegistry registry;
    struct Handle { unsigned id; } dlss{1}, fg{1}, unknown{2};
    require(!registry.find(0, &unknown), "unobserved creation cannot authorize NR");
    registry.created(0, &dlss, 13, 0xB, true);
    registry.created(0, &fg, 11, 0, false);
    require(registry.find(0, &dlss)->allows_nr(), "ray reconstruction allows NR");
    require(!registry.find(0, &fg)->allows_nr(), "same numeric Id cannot authorize FG");
    require(registry.find(0, &dlss)->flags == 0xB,
            "FG creation does not overwrite DLSS depth flags");

    registry.created(1, &dlss, 11, 0, false);
    require(!registry.find(1, &dlss)->allows_nr() && registry.find(0, &dlss)->allows_nr(),
            "provider-local handles remain separate even at the same address");
    registry.released(1, &dlss);
    require(!registry.find(1, &dlss) && registry.find(0, &dlss),
            "release retires only the matching provider");
    registry.released(0, &dlss);
    require(!registry.find(0, &dlss), "released address no longer authorizes NR");
    registry.created(0, &dlss, 11, 0, false);
    require(!registry.find(0, &dlss)->allows_nr() && !registry.find(0, &dlss)->flags_valid,
            "recycled DLSS address may become FG without inheriting flags");
    registry.created(0, &dlss, 1, 0, false);
    require(registry.find(0, &dlss)->allows_nr(), "super sampling allows NR without flags");
    for (unsigned feature : {0u, 4u, 11u, 12u, 18u, 32766u}) {
        registry.created(0, &unknown, feature, 0xB, true);
        require(!registry.find(0, &unknown)->allows_nr(),
                "DLSS-like flags cannot authorize other feature types");
    }
    std::puts("feature registry: all regression tests passed");
}
