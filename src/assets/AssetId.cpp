#include <quench/assets/AssetId.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <format>

namespace quench::assets {

AssetId assetId(std::string_view path) {
    AssetId id{XXH64(path.data(), path.size(), 0)};
    if (!id) id.value = 1; // 0 is the null ID
    return id;
}

std::string toString(AssetId id) { return std::format("{:016x}", id.value); }

} // namespace quench::assets
