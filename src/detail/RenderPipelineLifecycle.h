#pragma once

namespace ayt::render::detail {
class BGFXAdapter;
class RenderPipeline;

// Submission-owner operations on the built-in passes of a validated product
// pipeline (at most one instance per built-in type). These operations do not clear
// pass instances, change enabled/settings values, or shut down a borrowed UI
// backend. Custom pass owners still provide their own GPU cleanup.
// No device work is attempted with an uninitialized adapter.

// Release the Transparent/selection, Motion and Decal resources before rotating
// their borrowed scene/GBuffer/Lighting attachments. This also drops those
// passes' private geometry and program tokens via their existing cleanup.
void destroyAttachmentBorrowerResources(RenderPipeline& pipeline, BGFXAdapter& adapter);

// Main reset boundary: borrowers first, then Shadow/GBuffer/Lighting producers.
// TAA and exposure/history policies remain explicit at the reset call sites.
void destroyMainAttachmentResources(RenderPipeline& pipeline, BGFXAdapter& adapter);

// Complete built-in pass cleanup for pipeline replacement or Renderer shutdown.
// Borrowers precede producers. Shared program caches, FrameGraph/RT-pool targets,
// main scene FBO and Host-owned UI/backend state remain their owners' concern.
void destroyPipelineGpuResources(RenderPipeline& pipeline, BGFXAdapter& adapter);
}
