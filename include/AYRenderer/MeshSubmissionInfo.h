#pragma once

#include <AYMath/MathTypes.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::render {

/// Compact, immutable-after-publication description of one uploaded mesh section.
/// Indices address the mesh-wide GPU buffers. vertexOffset retains source metadata;
/// it does not change the renderer's existing global-index interpretation.
struct MeshSubmissionSection {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    uint32_t vertexOffset = 0;
    ayt::math::FVector3 localCenter{};
    std::vector<uint32_t> bonePalette;
};

/// CPU submission metadata published with the corresponding resource-uploaded GPU mesh.
/// A shared const snapshot may outlive reload/destroy, but owns no GPU resource or lease.
/// Slot strings remain source-relative; consumers resolve them against the mesh path.
/// No vertex/index arrays or L2 mesh objects are retained by this snapshot.
struct MeshSubmissionInfo {
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    std::vector<MeshSubmissionSection> sections;
    std::vector<std::string> materialSlots;
};

} // namespace ayt::render
