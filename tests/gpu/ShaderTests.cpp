#include "Test.h"

#include "test_shaders.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

using namespace quench;

QUENCH_TEST(shader_table) {
    const ShaderTable& table = shaders::test;
    CHECK(table.count == 3, "%zu entries", table.count);
    const ShaderBlob* vs = table.find("test_vs");
    const ShaderBlob* fs = table.find("test_fs");
    const ShaderBlob* cs = table.find("test_cs");
    CHECK(vs && vs->stage == ShaderStage::Vertex, "test_vs");
    CHECK(fs && fs->stage == ShaderStage::Fragment, "test_fs");
    CHECK(cs && cs->stage == ShaderStage::Compute, "test_cs");
    CHECK(table.find("missing") == nullptr, "found a missing entry");
}

QUENCH_TEST(shader_resources) {
    const ShaderBlob* vs = shaders::test.find("test_vs");
    const ShaderBlob* fs = shaders::test.find("test_fs");
    if (!vs || !fs) {
        CHECK(false, "entries missing");
        return;
    }
    const ShaderResources v = vs->resources, f = fs->resources;
    CHECK(v.samplers == 0 && v.storageTextures == 0 && v.storageBuffers == 0 && v.uniformBuffers == 1,
          "test_vs: %u samplers, %u storage textures, %u storage buffers, %u uniform buffers", v.samplers,
          v.storageTextures, v.storageBuffers, v.uniformBuffers);
    CHECK(f.samplers == 1 && f.storageTextures == 0 && f.storageBuffers == 0 && f.uniformBuffers == 1,
          "test_fs: %u samplers, %u storage textures, %u storage buffers, %u uniform buffers", f.samplers,
          f.storageTextures, f.storageBuffers, f.uniformBuffers);
    CHECK(v.readWriteStorageBuffers == 0 && v.threadCount[0] == 1, "test_vs: compute fields set");
}

QUENCH_TEST(shader_compute_resources) {
    const ShaderBlob* cs = shaders::test.find("test_cs");
    if (!cs) {
        CHECK(false, "test_cs missing");
        return;
    }
    const ShaderResources c = cs->resources;
    CHECK(c.samplers == 0 && c.storageBuffers == 1 && c.readWriteStorageBuffers == 2 && c.uniformBuffers == 1 &&
              c.storageTextures == 0 && c.readWriteStorageTextures == 0,
          "test_cs: %u samplers, %u storage buffers, %u read-write storage buffers, %u uniform buffers", c.samplers,
          c.storageBuffers, c.readWriteStorageBuffers, c.uniformBuffers);
    CHECK(c.threadCount[0] == 8 && c.threadCount[1] == 4 && c.threadCount[2] == 2, "test_cs: threads %u x %u x %u",
          c.threadCount[0], c.threadCount[1], c.threadCount[2]);
}

QUENCH_TEST(shader_formats) {
    for (const ShaderBlob& blob : shaders::test) {
        CHECK(blob.spirv && blob.spirv.size % 4 == 0, "%s: SPIR-V missing", blob.entry);
        if (blob.spirv) {
            std::uint32_t magic = 0;
            std::memcpy(&magic, blob.spirv.data, 4);
            CHECK(magic == 0x07230203u, "%s: SPIR-V magic %08x", blob.entry, magic);
        }
#if QUENCH_TEST_METAL
#if QUENCH_TEST_METALLIB
        CHECK(blob.metallib && blob.metallib.size > 4 && std::memcmp(blob.metallib.data, "MTLB", 4) == 0,
              "%s: metallib missing or without the MTLB magic", blob.entry);
        CHECK(!blob.msl, "%s: MSL source embedded next to the metallib", blob.entry);
#else
        CHECK(blob.msl && std::strstr(reinterpret_cast<const char*>(blob.msl.data), "metal_stdlib"),
              "%s: MSL source missing", blob.entry);
#endif
#endif
    }
}

// The depfile names both included files, so editing either one recompiles the shader.
QUENCH_TEST(shader_depfile) {
    std::ifstream in(std::string(QUENCH_TEST_SHADER_DIR) + "/test_fs.d");
    const std::string deps((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!deps.empty(), "test_fs.d missing");
    CHECK(deps.find("test_fs.spv:") != std::string::npos, "target missing: %s", deps.c_str());
    CHECK(deps.find("test.hlsl") != std::string::npos, "source missing: %s", deps.c_str());
    CHECK(deps.find("TestLocal.hlsli") != std::string::npos, "local include missing: %s", deps.c_str());
    CHECK(deps.find("include/TestLighting.hlsli") != std::string::npos, "include dir missing: %s", deps.c_str());
}
