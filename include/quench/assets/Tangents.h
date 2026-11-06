#pragma once

#include <quench/assets/Model.h>

#include <cstdint>
#include <vector>

// Tangents for normal maps: MikkTSpace (mikktspace.com), the tangent space of glTF and of the bakers that make normal
// maps, so that a baked map shades as it was baked. quench_cook runs it for glTF files without TANGENT; games run it
// for their own meshes (render::computeTangents for render::MeshData).
namespace quench::assets {

// Sets ModelVertex::tangent for an indexed triangle list from its positions, normals and uvs, in glTF's convention
// (the bitangent w * cross(normal, tangent) points toward -v). A vertex whose triangles need different tangents (a
// mirrored uv seam) is split; equal corners of one vertex stay one. Triangles without uv area get a tangent
// perpendicular to the normal. Returns, per vertex of the result, the index of the input vertex it came from, to carry
// other per-vertex data (skin weights) along; the order of the indices and of the triangles is kept, vertices no
// triangle uses are dropped.
std::vector<std::uint32_t> generateTangents(std::vector<ModelVertex>& vertices, std::vector<std::uint32_t>& indices);

} // namespace quench::assets
