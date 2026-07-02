#pragma once

#include "AYRenderTypes.h"

#include "IAYMaterial.h"
#include "IAYMesh.h"
#include "IAYTexture.h"

#include <string>

namespace ayt::render::detail
{

class RenderResourceManager;

std::string normalizeAssetPathKey(const std::string& path);

bool vertexLayoutFromMesh(const ayt::resource::IMesh& mesh, VertexLayoutDesc& out);

MeshHandle uploadMeshFromResource(RenderResourceManager& mgr,
                                  const ayt::resource::IMesh& mesh);

MaterialHandle bindMaterialFromResource(RenderResourceManager& mgr,
                                        const ayt::resource::IMaterial& material,
                                        const std::string& materialPath);

TextureHandle uploadTextureFromResource(RenderResourceManager& mgr,
                                          const ayt::resource::ITexture& texture,
                                          const std::string& cacheKey);

} // namespace ayt::render::detail
