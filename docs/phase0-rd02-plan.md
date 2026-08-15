# Phase 0 — RD-02 Implementation Plan

**Status:** Active
**Date:** 2026-07-06
**Owner:** Graphics engineer
**Phase 0 exit gate:** `RD-02` — `uploadMeshFromResource` preserves skin weights end-to-end.

## 0. Problem statement

`ENGINE-FOUNDATION-PLAN.md` §1.1 records:

> `RenderResourceManager: disk asset → GPU RenderMesh (partial: skin weights dropped at upload)`

Verified by reading the source:

| # | File:line | Issue |
|---|-----------|-------|
| 1 | `AYRenderer/include/AYRenderer/RenderTypes.h:49-55` | `VertexAttribute` enum has 5 entries — no `BoneIndices` / `BoneWeights` |
| 2 | `AYRenderer/include/AYRenderer/RenderTypes.h:80-83` | `VertexLayoutDesc` presets do not include skin channels |
| 3 | `AYRenderer/src/detail/VertexLayoutBridge.cpp:11-16` | `mapAttribute()` has no skin mapping |
| 4 | `AYRenderer/src/detail/RenderAssetBridge.cpp:129-163` | `vertexLayoutFromMesh()` checks 5 attributes, silently ignores `MeshAttribute::SkinWeight` |
| 5 | `AYRenderer/src/detail/RenderAssetBridge.cpp:202-255` | `repackMeshVertices()`'s `kChannels[]` skips skin |
| 6 | `AYRenderer/src/detail/RenderAssetBridge.cpp:257-310` | `uploadMeshFromResource()` calls bridge, drops skin |
| 7 | `AYRenderer/src/detail/GpuResources.h:13-18` | `GpuMesh` does not flag "this mesh has skin weights" |
| 8 | `AYRuntime/AYResource/src/AssetsImpl/AYMesh.cpp:69-107` | `_computeVertexStride()` does not add SkinWeight to stride |

Consequence: an `IMesh` with `hasSkinWeights() == true` reaches `RenderResourceManager::loadMesh()` and emerges with `GpuMesh::layout` that **does not** contain bone channels. The skin data is silently dropped before GPU upload.

## 1. Scope

Fix items 1-7. Item 8 is **out of scope for Phase 0** — `AYResource` is a sister module and adding a public `Mesh::addSkinWeightForTesting()` helper requires a public-API change in `AYResource`. We work around this in the unit test by constructing the `IMesh` layout using existing public setters + the new `IAYMesh` contract.

What this plan **does not** do (deferred):

- Skinning shader (`RD-03`).
- Bone matrix upload (`RD-04`).
- Skinned forward pass (`RD-05`).
- Per-vertex repack of skin weights to bgfx-format. The repack path here copies raw `VertexSkinWeight` bytes into the GPU vertex buffer interleaved at a known offset; the skinning shader (Phase 1) will consume them as `vec4 boneIndices` + `vec4 boneWeights`.

## 2. Target end state

```cpp
// AYRenderer/RenderTypes.h
enum class VertexAttribute : uint8_t {
    Position, Normal, TexCoord0, Tangent, Color0,
    BoneIndices,   // uint4 (4x u8)
    BoneWeights,   // float4 (4x f32)
};

// AYRenderAssetBridge::uploadMeshFromResource():
//   - When IMesh::hasSkinWeights() == true:
//       1. VertexLayoutDesc gets BoneIndices (Uint8x4 normalized) + BoneWeights (Floatx4) channels
//       2. Vertex stride grows by 24 bytes
//       3. repackMeshVertices copies IMesh::getSkinWeights() bytes into the new channels
//       4. GpuAYResource/AYResource/assetsImpl/Mesh.hasSkinWeights = true
//   - When IMesh::hasSkinWeights() == false:
//       Same behavior as today (regression-safe).
```

## 3. File-by-file change list

### 3.1 `AYRenderer/RenderTypes.h`

Add two enum entries and one preset.

```cpp
enum class VertexAttribute : uint8_t {
    Position,
    Normal,
    TexCoord0,
    Tangent,
    Color0,
    BoneIndices,   // <-- new
    BoneWeights,   // <-- new
};
```

Bump `kMaxElements` from 8 to 10 to keep room for future channels without churn.

Add a preset:

```cpp
static VertexLayoutDesc skinnedAddon();   // returns BoneIndices(4×u8) + BoneWeights(4×f32)
```

The full skinned layout is built dynamically in `RenderAssetBridge::vertexLayoutFromMesh` — there is no single "skinned PBR" preset; presets stay per-channel.

### 3.2 `AYRenderTypes.cpp`

Implement `VertexLayoutDesc::skinnedAddon()`.

### 3.3 `VertexLayoutBridge.cpp`

Add two branches in `mapAttribute()`:

```cpp
case VertexAttribute::BoneIndices: out = bgfx::Attrib::BoneIndices; return true;
case VertexAttribute::BoneWeights: out = bgfx::Attrib::BoneWeights; return true;
```

`BoneIndices` is `bgfx::AttribType::Uint8` normalized; `BoneWeights` is `bgfx::AttribType::Float`. Both already supported by `mapComponentType()`.

### 3.4 `RenderAssetBridge.cpp`

In `vertexLayoutFromMesh()`:

- If `mesh.hasAttribute(MeshAttribute::SkinWeight)`, append two elements to `out`:
  - `BoneIndices`, 4×Uint8, normalized=true
  - `BoneWeights`, 4×Float, normalized=false
- Order: skin channels come last (after Color). This is deterministic and lets `repackMeshVertices` extend the existing loop.

In `repackMeshVertices()`:

- Append a skin branch to `kChannels[]`:
  - `MeshAttribute::SkinWeight` + `bgfx::Attrib::BoneIndices` + 4 (×u8)
  - `MeshAttribute::SkinWeight` + `bgfx::Attrib::BoneWeights` + 4 (×f32)
- Inside the per-vertex loop, read from `IMesh::getSkinWeights()` and copy the appropriate sub-array.

### 3.5 `GpuResources.h`

Add to `GpuMesh`:

```cpp
struct GpuMesh {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle  indexBuffer  = BGFX_INVALID_HANDLE;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    bool     hasSkinWeights = false;       // <-- new
    VertexLayoutDesc layout{};
};
```

`vertexCount` also added because the existing `RenderResourceManager` did not store it (test queries it).

### 3.6 `RenderResourceManager.cpp`

In `uploadMeshInternal()`:

- Capture `mesh.hasSkinWeights` from `layout` — but `VertexLayoutDesc` does not currently expose a "has skin" query. We have two options:
  - (a) Pass a `bool hasSkinWeights` parameter through the call chain from `uploadMeshFromResource`.
  - (b) Derive it from the layout by inspecting elements for `BoneIndices` / `BoneWeights`.

We choose **(a)** because it's explicit and the layout-derived approach is fragile if other channels ever carry similar names.

In `uploadMeshFromResource()`:

- Compute `bool meshHasSkin = mesh.hasAttribute(MeshAttribute::SkinWeight);` before calling `uploadMeshInternal`.
- Pass it down through `createMeshFromResourceData` → `uploadMeshInternal`.
- After `uploadMeshInternal` returns, set `mesh.hasSkinWeights = meshHasSkin` in the `GpuMesh` entry. (`uploadMeshInternal` is the only place that constructs `GpuMesh`.)

### 3.7 `RenderResourceManager.h`

- `createMeshFromResourceData` signature gains `bool hasSkinWeights = false`.
- `uploadMeshInternal` signature gains `bool hasSkinWeights = false`.

## 4. Unit test (`AYRenderer/unittest/Test_SkinWeightUpload.cpp`)

Cover the contract:

1. **Layout stride** — `vertexLayoutFromMesh()` for a skinned mesh returns a layout whose `strideBytes()` equals position + normal + uv (32) + skin (24) = **56 bytes**.
2. **Layout channels** — layout contains one `BoneIndices` element (4×Uint8 normalized) and one `BoneWeights` element (4×Float).
3. **Repack correctness** — repacked bytes for a known skinned mesh contain the expected skin weight values at the expected offset.
4. **`GpuMesh::hasSkinWeights`** — calling `loadMesh()` on a skinned `.aymesh` results in `RenderResourceManager::meshes().at(h.id).hasSkinWeights == true`.

Test fixture: builds an `IMesh` via the public `Mesh` API. Since `Mesh` has no public `setSkinWeights()`, the test goes through binary serialization: construct minimal `Mesh` with cube data, then **manually edit the saved binary** to add a skin-weights block, then reload — exercise the full `IMesh::loadFromBinary` → bridge → repack path.

Alternative: **add a `Mesh::addSkinWeightsForTest(const std::vector<VertexSkinWeight>&)` helper** under `#ifdef AYRESOURCE_TEST_FRIENDLY` so the test can populate weights directly. This is preferred — it keeps the test deterministic and decoupled from the binary format.

We will take the **friend class approach** in the test: `class RenderSkinWeightTest : public ::testing::Test { friend class ayt::resource::Mesh; };` won't work cross-namespace. Better: declare `friend class AYRendererSkinWeightTestAccess;` in `Mesh`, then have the test fixture expose `setSkinWeights()` via that friend.

Implementation choice: **add a thin `Mesh` public method `void debugSetSkinWeights(const std::vector<VertexSkinWeight>&)`** guarded by `#ifndef AYRESOURCE_FINAL`. This is the standard escape hatch used elsewhere in the codebase.

We will discuss with the AYResource owner before merging.

## 5. Acceptance criteria

```cpp
TEST(SkinWeightUpload, preserves_through_bridge) {
    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);                  // pos+norm+uv, stride 32
    mesh.debugSetSkinWeights(skinData);    // 8 vertices × 4 bones
    mesh.setAttributeMask(mesh.getAttributeMask() | (1u << MeshAttribute::SkinWeight));

    ayt::render::VertexLayoutDesc layout;
    CHECK(ayt::render::detail::vertexLayoutFromMesh(mesh, layout));
    CHECK(layout.strideBytes() == 32u + 24u);   // 56

    bgfx::VertexLayout bgfxLayout;
    CHECK(ayt::render::detail::buildBgfxVertexLayout(layout, bgfxLayout));
    CHECK(bgfxLayout.getStride() == 56u);

    std::vector<uint8_t> repacked;
    CHECK(ayt::render::detail::repackMeshVertices(mesh, bgfxLayout, repacked));
    CHECK(repacked.size() == 56u * 8u);

    // BoneWeights offset should be after BoneIndices (4 bytes after VertexLayoutDesc order)
    // Position(12) + Normal(12) + UV(8) + BoneIndices(4) + BoneWeights(16) = 56
    const uint16_t bwOffset = bgfxLayout.getOffset(bgfx::Attrib::BoneWeights);
    CHECK(bwOffset == 32u + 4u);

    // Verify repacked weight bytes match the source
    const float* w0 = reinterpret_cast<const float*>(repacked.data() + bwOffset);
    CHECK(w0[0] == Approx(0.25f));  // whatever we set in skinData[0]
}
```

```cpp
TEST(SkinWeightUpload, gpu_mesh_flag_set_after_load) {
    // Save mesh to disk, load via RenderResourceManager (Noop backend),
    // assert GpuAYResource/AYResource/assetsImpl/Mesh.hasSkinWeights == true and GpuMesh.vertexCount == 8.
}
```

## 6. Out of Phase 0 (deferred to Phase 1)

| Item | Phase 1 task | Owner |
|------|--------------|-------|
| Bone matrix SSBO upload | `RD-04` | Graphics |
| Skinned forward pass | `RD-05` | Graphics |
| Skinning uniforms contract | `SH-01` | Graphics + Shader |
| `Mesh::debugSetSkinWeights()` merge upstream | (this PR) | Content + Graphics |

## 7. Risk

- **Test-friend method on `Mesh`** is a public-API surface change in `AYResource`. Mitigated by guard `#ifndef AYRESOURCE_FINAL`.
- **`VertexAttribute` enum growth** is ABI-visible (`uint8_t`). Adding new entries at the end is forward-compatible. No `static_cast` callers in the codebase iterate the enum, so safe.
- **`GpuMesh::hasSkinWeights`** is additive. No existing struct size-impact if appended last (current layout uses `bgfx::VertexBufferHandle` + `IndexBufferHandle` + `uint32_t` + `VertexLayoutDesc`).

## 8. Order of operations

1. Add `BoneIndices` / `BoneWeights` to `VertexAttribute` enum + bump `kMaxElements`.
2. Add `VertexLayoutDesc::skinnedAddon()`.
3. Add `mapAttribute()` branches in `VertexLayoutBridge`.
4. Extend `vertexLayoutFromMesh()` and `repackMeshVertices()` in `RenderAssetBridge`.
5. Extend `GpuMesh` and `uploadMeshFromResource()` plumbing.
6. Add `Mesh::debugSetSkinWeights()` (in `AYResource`).
7. Write `Test_SkinWeightUpload.cpp`.
8. Build & run.