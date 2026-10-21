#pragma once

#include <AYRenderer/MeshSubmissionInfo.h>
#include <AYResource/assetsDefs/IMesh.h>

namespace ayt::render::detail {

/// Pure CPU preparation. On failure output is unchanged and no GPU objects exist.
/// Bounds work and allocation independently of untrusted mesh counts/overlapping sections.
bool buildMeshSubmissionInfo(const ayt::resource::IMesh& mesh,
                             MeshSubmissionInfo& output, std::string* error = nullptr);

} // namespace ayt::render::detail
