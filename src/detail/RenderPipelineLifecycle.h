#pragma once

namespace ayt::render::detail {
class BGFXAdapter;
class RenderPipeline;

// Submission-owner operations on every mounted pass instance. Ownership phases
// are fixed by each pass constructor, not names or an enumerated type list.
// These operations do not clear instances, change enabled/settings values, or
// shut down a borrowed UI backend. Custom owners override destroyResources().
// No device work is attempted with an uninitialized adapter.

// Release attachment borrowers before rotating scene/GBuffer/Lighting targets.
// This also drops their private geometry/programs through the same cleanup hook.
void destroyAttachmentBorrowerResources(RenderPipeline& pipeline, BGFXAdapter& adapter);

// Main reset boundary: borrowers first, then Shadow/GBuffer/Lighting producers.
// TAA and exposure/history policies remain explicit at the reset call sites.
void destroyMainAttachmentResources(RenderPipeline& pipeline, BGFXAdapter& adapter);

// Complete pass cleanup for replacement or Renderer shutdown. Borrowers, then
// independent passes, then attachment owners; reverse dispatch within a phase.
// Shared program caches, FrameGraph/RT-pool targets,
// main scene FBO and Host-owned UI/backend state remain their owners' concern.
void destroyPipelineGpuResources(RenderPipeline& pipeline, BGFXAdapter& adapter);
}
