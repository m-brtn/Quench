# Credits

Basis Universal test textures of the assets module, generated for this repository; CC0 1.0 (public domain
dedication). Nothing here comes from another source.

Each file is a KTX2 file of one procedural image from `tests/assets/BasisTestImages.h` (`basisTestFiles()`) with its
full mip chain, encoded by `quench_cook`'s library (Basis Universal v2.50; ETC1S at quality 128, UASTC with Zstandard
supercompression):

| File                 | Image                                        | Codec | Transfer |
|----------------------|----------------------------------------------|-------|----------|
| `color_etc1s.ktx2`   | `basisColorImage(64, 64)`, opaque            | ETC1S | sRGB     |
| `alpha_etc1s.ktx2`   | `basisAlphaImage(64, 64)`, alpha             | ETC1S | sRGB     |
| `color_uastc.ktx2`   | `basisColorImage(64, 64)`, opaque            | UASTC | sRGB     |
| `alpha_uastc.ktx2`   | `basisAlphaImage(64, 64)`, alpha             | UASTC | sRGB     |
| `normals_uastc.ktx2` | `basisNormalImage(64, 64)`, a normal map     | UASTC | linear   |
| `odd_uastc.ktx2`     | `basisColorImage(6, 5)`, not a multiple of 4 | UASTC | sRGB     |
| `green_etc1s.ktx2`   | `basisSolidImage(16, 16, 20, 200, 20)`       | ETC1S | sRGB     |
| `blue_uastc.ktx2`    | `basisSolidImage(16, 16, 20, 20, 200)`       | UASTC | sRGB     |

`quench_cook_tests cook_basis_test_data` checks them against a fresh encode; `QUENCH_UPDATE_TEST_DATA=1` with the same
case writes them again. The tests embed the files (`tests/CMakeLists.txt`, `BasisTestData.cpp`).
