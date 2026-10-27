#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render {

inline constexpr uint32_t kResourceResidencyMaxTextureRows = 512;
inline constexpr uint32_t kResourceResidencyMaxKeysPerTexture = 4;
inline constexpr uint32_t kResourceResidencyMaxKeyBytes = 1024;

enum class ObservedTextureKind : uint8_t { Unknown, Texture2D, Cube };
enum class ObservedTextureFilter : uint8_t { Unknown, Point, Linear, AnisotropicDeviceMaximum };
enum class ObservedTextureAddress : uint8_t { Unknown, Repeat, Mirror, Clamp, Border };

enum class ObservedResourceKind : uint8_t { Mesh, Texture };

/// Current successful GPU version for one live Renderer handle. This is an
/// owner-thread diagnostic value, not an allocation lease or a VRAM bound.
/// Unknown logical bytes must never be treated as zero for budget admission.
struct ResourceLogicalPayloadObservation {
    uint64_t contextId = 0;
    uint64_t resourceId = 0;
    ObservedResourceKind kind = ObservedResourceKind::Mesh;
    bool borrowed = false;
    bool logicalBytesKnown = false;
    uint64_t logicalBytes = 0;
};

/// Owned diagnostic values for one current RRM table entry, not a GPU lease.
/// Descriptions refer to the last successfully created GPU version. Cache keys
/// are opaque identities and are not necessarily filesystem paths.
struct TextureResidencyRow {
    uint64_t textureId = 0;
    uint32_t width = 0, height = 0;
    ObservedTextureKind kind = ObservedTextureKind::Unknown;
    bool borrowed = false, dynamic = false;
    bool storageDescriptionKnown = false;
    std::string storageFormat;
    uint32_t mipCount = 0;
    bool srgbKnown = false, srgb = false;
    bool logicalStorageBytesKnown = false;
    uint64_t logicalStorageBytes = 0;
    bool creationUploadBytesKnown = false;
    uint64_t creationUploadBytes = 0;
    bool sourcePayloadKnown = false;
    std::string sourceFormat;
    uint32_t sourceMipCount = 0;
    uint64_t sourcePayloadBytes = 0;
    uint64_t retainedCpuImageBytes = 0;
    bool partialChainFallback = false, samplerFallback = false;
    // Creation defaults only. Draw-specific sampler overrides are not observed.
    ObservedTextureFilter minFilter = ObservedTextureFilter::Unknown;
    ObservedTextureFilter magFilter = ObservedTextureFilter::Unknown;
    ObservedTextureFilter mipFilter = ObservedTextureFilter::Unknown;
    ObservedTextureAddress addressU = ObservedTextureAddress::Unknown;
    ObservedTextureAddress addressV = ObservedTextureAddress::Unknown;
    ObservedTextureAddress addressW = ObservedTextureAddress::Unknown;
    uint64_t cacheKeyCount = 0;
    bool cacheKeysTruncated = false;
    std::vector<std::string> cacheKeys;
};

/// Explicit submission-owner snapshot; no loading, uploading, eviction or GPU
/// readback. Totals cover the complete RRM tables, even when rows are omitted.
/// Logical bytes exclude borrowed storage, pass/RT-pool/device-helper resources,
/// retired allocations and driver overhead. They are not total VRAM usage.
struct ResourceResidencySnapshot {
    bool available = false;
    uint64_t contextId = 0;
    uint64_t meshEntries = 0, materialEntries = 0, textureEntries = 0;
    // Current owned mesh buffer payloads, including optional wireframe indices.
    // These are logical upload lengths, not actual device allocations or peaks.
    uint64_t knownOwnedLogicalMeshBytes = 0;
    uint64_t ownedTextures = 0, borrowedTextures = 0;
    uint64_t knownOwnedLogicalTextureBytes = 0, unknownOwnedStorageTextures = 0;
    // Creation payload of current versions, not cumulative upload bandwidth.
    uint64_t knownOwnedCreationUploadBytes = 0, unknownOwnedUploadTextures = 0;
    uint64_t retainedCpuImageBytes = 0;
    uint64_t textureCacheKeys = 0, textureRowsOmitted = 0;
    std::vector<TextureResidencyRow> textures;
};

} // namespace ayt::render
