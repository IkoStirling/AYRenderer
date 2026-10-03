#pragma once

#include "detail/GpuResources.h"
#include "detail/RasterConvention.h"
#include "AYRenderer/RenderScene.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ayt::render::detail {

// Three affine columns, with translation in .w. No transpose/quantization or
// decomposition: nonuniform scale and shear retain their exact geometry.
struct RigidInstanceData { float columns[3][4]; };
static_assert(sizeof(RigidInstanceData) == 48);
struct InstancingStats { uint32_t draws = 0; uint32_t objects = 0; };

inline bool packRigidInstance(const DrawItem& item, const GpuMesh& mesh,
                              RigidInstanceData& output) {
    if (item.boneMatrices || item.jointCount || item.boneRemap || item.payload
        || item.particleBatch || item.outlineHull || mesh.hasSkinWeights
        || mesh.hasMorphTargets) return false;
    const auto& m = item.world;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        if (!std::isfinite(m(r,c))) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
        if (std::abs(m(r,c)) > 1e6f) return false;
    if (m(3,0) != 0 || m(3,1) != 0 || m(3,2) != 0 || m(3,3) != 1) return false;
    const double determinant =
        double(m(0,0)) * (double(m(1,1))*m(2,2) - double(m(1,2))*m(2,1))
      - double(m(0,1)) * (double(m(1,0))*m(2,2) - double(m(1,2))*m(2,0))
      + double(m(0,2)) * (double(m(1,0))*m(2,1) - double(m(1,1))*m(2,0));
    double product = 1;
    for (int c = 0; c < 3; ++c)
        product *= std::sqrt(double(m(0,c))*m(0,c) + double(m(1,c))*m(1,c)
                            + double(m(2,c))*m(2,c));
    // Fail closed for singular/extreme transforms; normal cofactor evaluation
    // would otherwise overflow or divide by zero on the GPU.
    if (!std::isfinite(product) || std::abs(determinant) < 1e-8
        || std::abs(determinant) < product * 1e-5 || product > 1e18) return false;
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) output.columns[c][r] = m(r,c);
        output.columns[c][3] = m(c,3);
    }
    return true;
}

inline size_t collectRigidInstances(const std::vector<const DrawItem*>& items,
    size_t first, const GpuMesh& mesh, size_t limit,
    std::vector<RigidInstanceData>& output) {
    output.clear();
    if (first >= items.size() || !items[first]) return 0;
    const DrawItem& key = *items[first];
    for (size_t i = first; i < items.size() && output.size() < limit; ++i) {
        if (!items[i]) break;
        const DrawItem& candidate = *items[i];
        if (candidate.mesh.id != key.mesh.id || candidate.material.id != key.material.id
            || candidate.firstIndex != key.firstIndex || candidate.indexCount != key.indexCount
            || candidate.shadowFlags != key.shadowFlags || candidate.sortKey != key.sortKey
            || reversesWinding(candidate.world) != reversesWinding(key.world)) break;
        RigidInstanceData data{};
        if (!packRigidInstance(candidate, mesh, data)) break;
        output.push_back(data);
    }
    return output.size();
}

// Only Renderer-owned built-ins are specialized. Arbitrary material source is
// never rewritten. All variants still enter Phoskia -> IR -> shaderc -> pool.
inline std::string rigidInstancedSource(const char* source, bool shadow = false) {
    std::string result(source);
    const size_t start = result.find("vertex {");
    const size_t open = result.find('{', start);
    if (start == std::string::npos || open == std::string::npos) return {};
    size_t end = open + 1; int depth = 1;
    for (; end < result.size() && depth; ++end) {
        if (result[end] == '{') ++depth;
        if (result[end] == '}') --depth;
    }
    if (depth) return {};
    const std::string world = "(((ix.xyz * pos.x) + (iy.xyz * pos.y)) + ((iz.xyz * pos.z) + vec3(ix.w, iy.w, iz.w)))";
    const std::string clip = "(viewProjectionMatrix * vec4(" + world + ", 1.0))";
    std::string vertex = "vertex {\n in pos : position\n in ix : instance0\n in iy : instance1\n in iz : instance2\n";
    if (shadow) {
        vertex += "out clipZw : texcoord = vec2(" + clip + ".z, " + clip + ".w)\n";
    } else {
        vertex += R"(
            in nrm : normal
            in tan : tangent
            in uv : texcoord
            out worldNormal : normal = (((cross(iy.xyz, iz.xyz) * nrm.x)
                + (cross(iz.xyz, ix.xyz) * nrm.y)) + (cross(ix.xyz, iy.xyz) * nrm.z))
                / dot(ix.xyz, cross(iy.xyz, iz.xyz))
            out worldTangent : tangent = vec4(((ix.xyz * tan.x) + (iy.xyz * tan.y)) + (iz.xyz * tan.z), tan.w)
            out vUv : texcoord = uv
        )";
        vertex += "out worldPos : position = " + world + "\n";
    }
    vertex += "return " + clip + "\n}";
    result.replace(start, end-start, vertex);
    return result;
}
} // namespace ayt::render::detail
