#include <doctest/doctest.h>
#include "generator/generator.h"
#include "generator/volume_set.h"
#include "shade/oct.h"
#include "world/override_store.h"
#include "world/brick.h"
#include <cmath>
#include <vector>

using namespace ve;

TEST_CASE("oct snorm8 dense sphere round-trips with dot>0.999") {
    for (int x = -16; x <= 16; x++) {
        for (int y = -16; y <= 16; y++) {
            for (int z = -16; z <= 16; z++) {
                if (x == 0 && y == 0 && z == 0) continue;
                float n[3] = {float(x), float(y), float(z)};
                float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
                n[0] /= len; n[1] /= len; n[2] /= len;
                uint16_t packed = oct_encode_snorm8(n);
                float dec[3];
                oct_decode_snorm8(packed, dec);
                float dot = n[0]*dec[0] + n[1]*dec[1] + n[2]*dec[2];
                CHECK(dot > 0.999f);
                int8_t bx = int8_t(packed & 0xFF);
                int8_t by = int8_t((packed >> 8) & 0xFF);
                CHECK(bx != -128);
                CHECK(by != -128);
            }
        }
    }
}

TEST_CASE("VolumeData valid with malformed normal_oct payload") {
    VolumeData data;
    data.dim = 4;
    data.sdf.assign(64, encode_sdf(1.0f));
    data.mat.assign(64, 0);
    CHECK(data.valid());
    CHECK_FALSE(data.has_normals());
    data.normal_oct.assign(63, 0);
    CHECK_FALSE(data.valid());
    float up[3] = {0, 1, 0};
    data.normal_oct.assign(64, oct_encode_snorm8(up));
    CHECK(data.valid());
    CHECK(data.has_normals());
}

TEST_CASE("sample_volume_gradient_lattice interpolates stored compact normals") {
    const int dim = 2;
    const float origin[3] = {0, 0, 0};
    const float voxel = 1.0f;
    std::vector<uint8_t> sdf(8, encode_sdf(-0.1f));
    std::vector<uint8_t> mat(8, 2);
    std::vector<uint16_t> normals(8);
    for (int i=0;i<8;i++) {
        int z = i / 4;
        float n[3] = { z==0 ? 1.0f : 0.0f, z==0 ? 0.0f : 1.0f, 0.0f };
        normals[i] = oct_encode_snorm8(n);
    }
    FieldSample out{};
    bool ok = sample_volume_gradient_lattice(sdf.data(), mat.data(), normals.data(), dim, origin, voxel, 0.5f, 0.5f, 0.5f, &out);
    CHECK(ok);
    CHECK(out.exact_gradient);
    float expected[3] = {0.70710678f, 0.70710678f, 0.0f};
    float dot = out.gradient[0]*expected[0] + out.gradient[1]*expected[1] + out.gradient[2]*expected[2];
    CHECK(dot > 0.99f);
    FieldSample out2{};
    bool ok2 = sample_volume_gradient_lattice(sdf.data(), mat.data(), nullptr, dim, origin, voxel, 0.5f, 0.5f, 0.5f, &out2);
    CHECK(ok2);
    CHECK_FALSE(out2.exact_gradient);
    FieldSample out3{};
    bool ok3 = sample_volume_gradient_lattice(sdf.data(), mat.data(), normals.data(), dim, origin, voxel, 5.0f, 0.5f, 0.5f, &out3);
    CHECK(ok3);
    CHECK(out3.exact_gradient);
    CHECK(out3.gradient[0] == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("resample_volume rotates stored compact normals") {
    const float origin[3] = {0,0,0};
    VolumeData src;
    src.dim = 8;
    const int src_n = 8*8*8;
    src.sdf.assign(src_n, encode_sdf(-0.1f));
    src.mat.assign(src_n, 1);
    float n_src[3] = {1,0,0};
    uint16_t enc = oct_encode_snorm8(n_src);
    src.normal_oct.assign(src_n, enc);
    src.solid_voxels = src_n;
    EditOp src_op = make_volume_add(0, origin, 0.05f, 8);
    const float basis[9] = {0,0,1, 0,1,0, -1,0,0};
    const float at[3] = {10,0,10};
    VolumeData out;
    EditOp out_op{};
    bool ok = resample_volume(src, src_op, basis, at, 1, kIslandDim, &out, &out_op);
    REQUIRE(ok);
    CHECK(out.has_normals());
    CHECK(static_cast<int>(out.normal_oct.size()) == kIslandDim*kIslandDim*kIslandDim);
    float src_centre[3] = {origin[0] + 0.5f * float(8-1) * 0.05f, origin[1] + 0.5f * float(8-1) * 0.05f, origin[2] + 0.5f * float(8-1) * 0.05f};
    float world_centre[3] = {
        basis[0]*src_centre[0] + basis[1]*src_centre[1] + basis[2]*src_centre[2] + at[0],
        basis[3]*src_centre[0] + basis[4]*src_centre[1] + basis[5]*src_centre[2] + at[1],
        basis[6]*src_centre[0] + basis[7]*src_centre[1] + basis[8]*src_centre[2] + at[2]
    };
    FieldSample gs{};
    const uint16_t *norm_ptr = out.normal_oct.data();
    bool ok2 = sample_volume_gradient_lattice(out.sdf.data(), out.mat.data(), norm_ptr, out.dim, out_op.pos, out_op.radius, world_centre[0], world_centre[1], world_centre[2], &gs);
    REQUIRE(ok2);
    float expected[3] = {0,0,-1};
    float dot = gs.gradient[0]*expected[0] + gs.gradient[1]*expected[1] + gs.gradient[2]*expected[2];
    CHECK(dot > 0.999f);
}

TEST_CASE("override returns compact normal rather than differentiating R8") {
    OverrideStore store(4);
    IVec3 brick{0,0,0};
    int slot = store.acquire(brick);
    REQUIRE(slot >= 0);
    OverrideBrick *b = store.data(slot);
    for (int i=0;i<kBrickSdfCount;i++) b->sdf[i] = encode_sdf(-0.1f);
    for (int i=0;i<kBrickVoxelCount;i++) b->mat[i] = 1;
    float n[3] = {0,1,0};
    uint16_t enc = oct_encode_snorm8(n);
    b->normal_oct.assign(kBrickSdfCount, enc);
    float x = 0.4f, y = 0.4f, z = 0.4f;
    FieldSample out{};
    bool ok = store.sample_gradient(x,y,z, &out);
    CHECK(ok);
    CHECK(out.exact_gradient);
    float dot = out.gradient[0]*0.0f + out.gradient[1]*1.0f + out.gradient[2]*0.0f;
    CHECK(dot > 0.999f);
    OverrideStore store2(4);
    int slot2 = store2.acquire(brick);
    REQUIRE(slot2 >=0);
    OverrideBrick *b2 = store2.data(slot2);
    for (int i=0;i<kBrickSdfCount;i++) b2->sdf[i] = encode_sdf(-0.1f);
    for (int i=0;i<kBrickVoxelCount;i++) b2->mat[i] = 1;
    FieldSample out2{};
    bool ok2 = store2.sample_gradient(x,y,z, &out2);
    CHECK_FALSE(ok2);
    b2->normal_oct.assign(kBrickSdfCount-1, enc);
    FieldSample out3{};
    CHECK_FALSE(store2.sample_gradient(x,y,z, &out3));
}
