// Test_Forward2DOpaque.cpp — CM-1 (2026-08-11) 2D lane ship.
//
// Pins the CM-1 contract (AY2D engine integration, 刀 1):
//   1) RenderPassSlot::Forward2DOpaque enum value = 14 (append-only,
//      EditorOverlay=13 was the previous max) + full table mirror.
//   2) makeDefault() includes the slot between ForwardOpaque and
//      Transparent (9 slots, was 8); makeDeferred() does NOT include
//      it (2D is Forward-path-only).
//   3) DrawItem.payload defaults to nullptr (pre-CM-1 behavior);
//      a payload survives the RenderScene round-trip.
//   4) Forward2DOpaquePass on an uninitialized adapter returns 0
//      (mirror Test_FO_Trans_NoopGate UB fix).
//   5) Lane split (sticky-Noop backend, logical draw counting):
//      - empty scene → 0 draws (zero behavior change pin)
//      - 1 payload item → exactly 1 draw (FO skips payload items,
//        Transparent skips Opaque materials — double draw would be 2)
//      - 1 cube item (no payload) → 1 draw (FO owns it; 2D lane skips)
//      - mixed cube + payload quad → 2 draws (both lanes, no overlap)
//   6) createUnitQuad returns a valid handle on an initialized
//      Noop renderer.
//   7) Minimal 2D closed loop: quad + Tilemap2D material (compiled
//      from kTilemapPhoskiaSource — this is the CM-4 runtime-compile
//      path) + checkerboard albedo texture + payload → exactly 1 draw.
//   8) DrawPayload2D is a 16-aligned POD (MSVC SIMD alignment; never
//      pack).

#include "AYRenderer.h"
#include "AYRenderScene.h"
#include "AYRenderTypes.h"
#include "AYTest.h"
#include "AYTilemapShaderSources.h"

#include "detail/Forward2DOpaquePass.h"
#include "detail/FrameContext.h"
#include "detail/GpuResources.h"
#include "detail/PassExecContext.h"

#include <sys/stat.h>

#include <cstdint>
#include <iostream>
#include <string>

// TEST_SUITE expands to `namespace _X_<name>`, which nests in the
// global namespace — file-scope using declarations (below) are what
// make the unqualified names visible inside it (S1b mirror).
using ayt::render::Backend;
using ayt::render::DrawItem;
using ayt::render::DrawPayload2D;
using ayt::render::kTilemapPhoskiaSource;
using ayt::render::RenderPassSlot;
using ayt::render::RenderPath;
using ayt::render::RenderPipelineDesc;
using ayt::render::RenderScene;
using ayt::render::detail::BGFXAdapter;
using ayt::render::detail::Forward2DOpaquePass;
using ayt::render::detail::FrameContext;
using ayt::render::detail::GpuMaterial;
using ayt::render::detail::GpuMesh;
using ayt::render::detail::GpuTexture;
using ayt::render::detail::PassExecContext;

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace {

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool shadercAvailable()
{
    return fileExists(AY_SHADER_SHADERC_HINT);
}

// 8x8 RGBA8 checkerboard for the closed-loop texture bind.
void fillCheckerboard(uint8_t* pixels, uint32_t width, uint32_t height, uint32_t cell)
{
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t* p = pixels + (y * width + x) * 4;
            if (dark) {
                p[0] = 40; p[1] = 60; p[2] = 200; p[3] = 255;
            } else {
                p[0] = 200; p[1] = 190; p[2] = 60; p[3] = 255;
            }
        }
    }
}

} // namespace

TEST_SUITE(Renderer2DLaneTests)

// === 1. Slot enum value + ABI =======================================

TEST_CASE(cm1_renderpassslot_forward2dopaque_value_is_14) {
    // ABI append-only: 14 follows EditorOverlay=13. Existing values
    // do NOT reorder (mirror Test_BloomBlur_S1b pin style).
    CHECK(static_cast<uint8_t>(RenderPassSlot::Shadow)        == 0);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Skybox)        == 1);
    CHECK(static_cast<uint8_t>(RenderPassSlot::ForwardOpaque) == 2);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Transparent)   == 3);
    CHECK(static_cast<uint8_t>(RenderPassSlot::PostProcess)   == 4);
    CHECK(static_cast<uint8_t>(RenderPassSlot::UI)            == 5);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBuffer)       == 6);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Lighting)      == 7);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomExtract)  == 8);
    CHECK(static_cast<uint8_t>(RenderPassSlot::BloomBlur)     == 9);
    CHECK(static_cast<uint8_t>(RenderPassSlot::DepthHaze)     == 10);
    CHECK(static_cast<uint8_t>(RenderPassSlot::SSAO)          == 11);
    CHECK(static_cast<uint8_t>(RenderPassSlot::GBufferDebug)  == 12);
    CHECK(static_cast<uint8_t>(RenderPassSlot::EditorOverlay) == 13);
    CHECK(static_cast<uint8_t>(RenderPassSlot::Forward2DOpaque) == 14);
}

// === 2. Pipeline slot position ======================================

TEST_CASE(cm1_make_default_includes_forward2dopaque_between_fo_and_transparent) {
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDefault();
    CHECK(desc.path == RenderPath::Forward);
    CHECK(desc.passes.size() == 9);  // 8 pre-CM-1 + Forward2DOpaque
    CHECK(desc.passes[0] == RenderPassSlot::Shadow);
    CHECK(desc.passes[1] == RenderPassSlot::ForwardOpaque);
    CHECK(desc.passes[2] == RenderPassSlot::Forward2DOpaque);
    CHECK(desc.passes[3] == RenderPassSlot::Transparent);
    CHECK(desc.passes[4] == RenderPassSlot::BloomExtract);
    CHECK(desc.passes[5] == RenderPassSlot::BloomBlur);
    CHECK(desc.passes[6] == RenderPassSlot::DepthHaze);
    CHECK(desc.passes[7] == RenderPassSlot::PostProcess);
    CHECK(desc.passes[8] == RenderPassSlot::UI);
    CHECK(desc.contains(RenderPassSlot::Forward2DOpaque));
}

TEST_CASE(cm1_make_deferred_omits_forward2dopaque) {
    // 2D is Forward-path-only (slot comment in AYRenderTypes.h).
    const RenderPipelineDesc desc = RenderPipelineDesc::makeDeferred();
    CHECK(desc.path == RenderPath::Deferred);
    CHECK(!desc.contains(RenderPassSlot::Forward2DOpaque));
    CHECK(desc.passes.size() == 12);  // unchanged vs pre-CM-1
}

// === 3. Payload default + round-trip ================================

TEST_CASE(cm1_drawitem_payload_defaults_nullptr_and_roundtrips) {
    DrawItem item;
    CHECK(item.payload == nullptr);

    DrawPayload2D payload;
    payload.sourceRectMin = ayt::math::FVector2(0.25f, 0.5f);
    payload.sourceRectMax = ayt::math::FVector2(0.75f, 1.0f);
    payload.tintRGBA      = ayt::math::FVector4(0.9f, 0.8f, 0.7f, 0.6f);
    payload.flip          = 2;
    payload.packedSortKey = 0x03000012;

    item.payload = &payload;
    RenderScene scene;
    scene.add(item);

    CHECK(scene.items().size() == 1u);
    const DrawItem& back = scene.items()[0];
    CHECK(back.payload == &payload);
    CHECK(back.payload->packedSortKey == 0x03000012u);
    CHECK(back.payload->flip == 2);
    CHECK_FLOAT_EQ(back.payload->sourceRectMin.x, 0.25f, 1e-5f);
    CHECK_FLOAT_EQ(back.payload->sourceRectMin.y, 0.5f, 1e-5f);
    CHECK_FLOAT_EQ(back.payload->sourceRectMax.x, 0.75f, 1e-5f);
    CHECK_FLOAT_EQ(back.payload->sourceRectMax.y, 1.0f, 1e-5f);
    CHECK_FLOAT_EQ(back.payload->tintRGBA.w, 0.6f, 1e-5f);
}

TEST_CASE(cm1_drawpayload2d_is_16byte_aligned_pod) {
    // MSVC SIMD alignment: FVector2/FVector4 are __m128-backed.
    // NEVER #pragma pack; the natural layout must keep alignof 16.
    CHECK(alignof(DrawPayload2D) == 16);
    CHECK(sizeof(DrawPayload2D) >= 48);
}

// === 4. Pass gate on uninitialized adapter ==========================

TEST_CASE(cm1_forward2dopaque_uninit_adapter_returns_zero) {
    // Mirror Test_FO_Trans_NoopGate: raw bgfx setViewTransform on an
    // uninitialized adapter is UB; must return 0 draws without
    // crashing.
    Forward2DOpaquePass pass;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    FrameContext frame{};
    std::unordered_map<uint64_t, GpuMesh> meshes;
    std::unordered_map<uint64_t, GpuTexture> textures;
    std::unordered_map<uint64_t, GpuMaterial> materials;
    RenderScene scene;
    PassExecContext ctx{
        adapter, pool, scene, meshes, textures, materials,
        0, 0, 1280, 720, frame, /*viewId=*/0
    };
    CHECK(pass.execute(ctx) == 0);
}

// === 5. Lane split (logical draw counting, sticky-Noop) =============

TEST_CASE(cm1_empty_scene_zero_draws) {
    // Zero behavior change pin: a scene with no payload items makes
    // the 2D lane contribute nothing.
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::RenderScene scene;
    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    CHECK(renderer.getFrameStats().drawCalls == 0u);
    renderer.shutdown();
}

TEST_CASE(cm1_3d_item_stays_in_forward_opaque_lane) {
    // A plain 3D item (payload == nullptr) is owned by
    // ForwardOpaquePass exactly once; the 2D lane skips it.
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(
            R"(material Unlit {
                property baseColor = vec4(1.0, 1.0, 1.0, 1.0);
                vertex {
                    in  position : position;
                    out position : position;
                    return vec4(position, 1.0);
                }
                fragment {
                    in  position : position;
                    return baseColor;
                }
            })",
            "cm1_3d_unlit");
    if (!material.isValid()) {
        std::cerr << "[Renderer test] SKIP: material acquire failed.\n";
        renderer.shutdown();
        return;
    }
    ayt::render::MeshHandle cube = renderer.createUnitCube();
    CHECK(cube.isValid());

    ayt::render::RenderScene scene;
    scene.add(cube, material);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    CHECK(renderer.getFrameStats().drawCalls == 1u);
    renderer.destroyMesh(cube);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_CASE(cm1_payload_item_drawn_exactly_once) {
    // 2D material stays BlendMode::Opaque (default): FO skips payload
    // items, Transparent skips Opaque materials, Shadow skips payload
    // casters ⇒ exactly 1 draw. A double-submit would be 2.
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle material =
        renderer.createMaterialFromPhoskia(kTilemapPhoskiaSource, "cm1_tilemap2d");
    if (!material.isValid()) {
        std::cerr << "[Renderer test] SKIP: Tilemap2D material compile failed.\n";
        renderer.shutdown();
        return;
    }
    ayt::render::MeshHandle quad = renderer.createUnitQuad();
    CHECK(quad.isValid());

    uint8_t pixels[8 * 8 * 4];
    fillCheckerboard(pixels, 8, 8, 4);
    ayt::render::TextureHandle tex =
        renderer.createTextureFromRgba8(8, 8, pixels, "cm1_checker");
    CHECK(tex.isValid());
    renderer.setMaterialTexture(material, "albedoMap", tex);

    DrawPayload2D payload;
    payload.sourceRectMin = ayt::math::FVector2(0.0f, 0.0f);
    payload.sourceRectMax = ayt::math::FVector2(1.0f, 1.0f);
    payload.packedSortKey = 0x01000000u;

    ayt::render::RenderScene scene;
    DrawItem item;
    item.mesh     = quad;
    item.material = material;
    item.payload  = &payload;
    scene.add(item);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    CHECK(renderer.getFrameStats().drawCalls == 1u);
    CHECK(renderer.getFrameStats().sceneItems == 1u);

    renderer.destroyTexture(tex);
    renderer.destroyMesh(quad);
    renderer.destroyMaterial(material);
    renderer.shutdown();
}

TEST_CASE(cm1_mixed_scene_split_lanes) {
    // Cube (no payload → FO) + quad (payload → 2D lane) = 2 draws,
    // no overlap. A lane confusion (cube in 2D lane or quad in FO)
    // would report 3.
    if (!shadercAvailable()) {
        std::cerr << "[Renderer test] SKIP: shaderc not available.\n";
        return;
    }
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 800;
    desc.height  = 600;
    CHECK(renderer.initialize(desc));

    ayt::render::MaterialHandle unlit =
        renderer.createMaterialFromPhoskia(
            R"(material Unlit {
                property baseColor = vec4(1.0, 1.0, 1.0, 1.0);
                vertex {
                    in  position : position;
                    out position : position;
                    return vec4(position, 1.0);
                }
                fragment {
                    in  position : position;
                    return baseColor;
                }
            })",
            "cm1_3d_unlit2");
    ayt::render::MaterialHandle tilemap =
        renderer.createMaterialFromPhoskia(kTilemapPhoskiaSource, "cm1_tilemap2d2");
    if (!unlit.isValid() || !tilemap.isValid()) {
        std::cerr << "[Renderer test] SKIP: material acquire failed.\n";
        renderer.shutdown();
        return;
    }
    ayt::render::MeshHandle cube = renderer.createUnitCube();
    ayt::render::MeshHandle quad = renderer.createUnitQuad();
    CHECK(cube.isValid());
    CHECK(quad.isValid());

    uint8_t pixels[8 * 8 * 4];
    fillCheckerboard(pixels, 8, 8, 4);
    ayt::render::TextureHandle tex =
        renderer.createTextureFromRgba8(8, 8, pixels, "cm1_checker2");
    renderer.setMaterialTexture(tilemap, "albedoMap", tex);

    DrawPayload2D payload;
    payload.packedSortKey = 0x02000005u;

    ayt::render::RenderScene scene;
    DrawItem cubeItem;
    cubeItem.mesh     = cube;
    cubeItem.material = unlit;
    scene.add(cubeItem);

    DrawItem quadItem;
    quadItem.mesh     = quad;
    quadItem.material = tilemap;
    quadItem.payload  = &payload;
    scene.add(quadItem);

    renderer.beginFrame({});
    renderer.render(scene);
    renderer.endFrame();

    CHECK(renderer.getFrameStats().drawCalls == 2u);
    CHECK(renderer.getFrameStats().sceneItems == 2u);

    renderer.destroyTexture(tex);
    renderer.destroyMesh(cube);
    renderer.destroyMesh(quad);
    renderer.destroyMaterial(unlit);
    renderer.destroyMaterial(tilemap);
    renderer.shutdown();
}

// === 6. createUnitQuad ==============================================

TEST_CASE(cm1_create_unit_quad_valid) {
    ayt::render::Renderer renderer;
    ayt::render::InitDesc desc;
    desc.backend = ayt::render::Backend::Noop;
    desc.width   = 640;
    desc.height  = 480;
    CHECK(renderer.initialize(desc));

    ayt::render::MeshHandle quad = renderer.createUnitQuad();
    CHECK(quad.isValid());
    renderer.destroyMesh(quad);
    renderer.shutdown();
}

TEST_SUITE_END
