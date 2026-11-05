#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quench::assets {

// The identity of an asset: XXH64 (seed 0) of its path as quench::fs takes it ("models/crate.qmodel"). The same on
// every platform and in quench_cook, so one cooked file can refer to another by path and both sides agree on the ID.
struct AssetId {
    std::uint64_t value = 0; // 0: null

    explicit operator bool() const { return value != 0; }
    friend auto operator<=>(AssetId, AssetId) = default;
};

AssetId assetId(std::string_view path);
// 16 lowercase hex digits.
std::string toString(AssetId id);

struct AssetIdHash {
    std::size_t operator()(AssetId id) const { return std::size_t(id.value); }
};

} // namespace quench::assets
