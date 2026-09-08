#pragma once

#include <cstdint>
#include <map>
#include <utility>

namespace nr {

// NGX handle Id is not a feature type, nor unique across providers. Keep the
// creation contract by provider and opaque handle address, until ReleaseFeature.
class FeatureRegistry {
public:
    struct Contract {
        unsigned feature;
        unsigned flags;
        bool flags_valid;
        bool reported = false;
        bool allows_nr() const { return feature == 1 || feature == 13; }
    };

    void created(unsigned provider, const void *handle, unsigned feature,
                 unsigned flags, bool flags_valid) {
        if (handle) entries_[{provider, reinterpret_cast<std::uintptr_t>(handle)}] =
            {feature, flags, flags_valid, false};
    }

    Contract *find(unsigned provider, const void *handle) {
        auto it = entries_.find({provider, reinterpret_cast<std::uintptr_t>(handle)});
        return it == entries_.end() ? nullptr : &it->second;
    }

    void released(unsigned provider, const void *handle) {
        entries_.erase({provider, reinterpret_cast<std::uintptr_t>(handle)});
    }

private:
    std::map<std::pair<unsigned, std::uintptr_t>, Contract> entries_;
};

} // namespace nr
