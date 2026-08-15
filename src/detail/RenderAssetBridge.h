#pragma once

#include "AYRenderer/RenderTypes.h"

#include "AYResource/assetsDefs/IMaterial.h"
#include "AYResource/assetsDefs/IMesh.h"
#include "AYResource/assetsDefs/ITexture.h"

#include <bgfx/bgfx.h>

#include <string>
#include <vector>

namespace ayt::render::detail
{

class RenderResourceManager;

std::string normalizeAssetPathKey(const std::string& path);

bool vertexLayoutFromMesh(const ayt::resource::IMesh& mesh, VertexLayoutDesc& out);

bool buildBgfxVertexLayoutFromMesh(const ayt::resource::IMesh& mesh,
                                   VertexLayoutDesc& desc,
                                   bgfx::VertexLayout& bgfxLayout);

bool meshNeedsVertexRepack(const ayt::resource::IMesh& mesh,
                           const bgfx::VertexLayout& bgfxLayout);

bool repackMeshVertices(const ayt::resource::IMesh& mesh,
                        const bgfx::VertexLayout& bgfxLayout,
                        std::vector<uint8_t>& out);

MeshHandle uploadMeshFromResource(RenderResourceManager& mgr,
                                  const ayt::resource::IMesh& mesh);

MaterialHandle bindMaterialFromResource(RenderResourceManager& mgr,
                                        const ayt::resource::IMaterial& material,
                                        const std::string& materialPath);

TextureHandle uploadTextureFromResource(RenderResourceManager& mgr,
                                          const ayt::resource::ITexture& texture,
                                          const std::string& cacheKey);

} // namespace ayt::render::detail
