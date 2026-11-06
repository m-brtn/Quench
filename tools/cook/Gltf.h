#pragma once

// glTF reading shared by the model and the animation cook.

#include <quench/core/Result.h>

#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace quench::cook {

// Reads a glTF or GLB file and its external buffers (read here, not by fastgltf, so that every file read is added to
// `inputs` for the depfile) and validates it.
Result<fastgltf::Asset> loadGltf(const std::filesystem::path& source, fastgltf::Options options,
                                 std::vector<std::filesystem::path>& inputs);

std::string_view gltfError(fastgltf::Error error);
glm::mat4 toGlm(const fastgltf::math::fmat4x4& m);

// The name of every node as a joint: its own where it is unique and not empty, else "node<index>". The model and the
// animation cook name joints alike, which is what binds a model's skins to a skeleton at run time.
std::vector<std::string> jointNames(const fastgltf::Asset& asset);

// A node's local transform (TRS or a matrix decomposed).
struct NodeTransform {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};
NodeTransform nodeTransform(const fastgltf::Node& node);
// Every node's parent (-1 for roots of the scene graph).
std::vector<int> nodeParents(const fastgltf::Asset& asset);

} // namespace quench::cook
