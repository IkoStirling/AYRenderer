#include "MeshSubmissionInfoBuilder.h"

#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace ayt::render::detail {
namespace {
constexpr uint64_t kInputBytes = 256ull * 1024 * 1024;
constexpr uint64_t kMetadataBytes = 16ull * 1024 * 1024;
constexpr uint64_t kCenterIndexVisits = 64ull * 1024 * 1024;
constexpr uint32_t kMaterialSlots = 65536;
constexpr size_t kSlotStringBytes = 65536;

bool fail(std::string* error, const char* message) {
    if(error)*error=message;
    return false;
}
bool finite(const ayt::math::FVector3& value) {
    return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);
}
}

bool buildMeshSubmissionInfo(const ayt::resource::IMesh& mesh,
                             MeshSubmissionInfo& output,std::string* error)
{
    using ayt::resource::MeshAttribute;
    const uint32_t vertexCount=mesh.getVertexCount(), indexCount=mesh.getIndexCount();
    const uint32_t stride=mesh.getVertexStride();
    if(!vertexCount||!indexCount||!stride||!mesh.getVertexData()||!mesh.getIndexData())
        return fail(error,"mesh submission requires nonempty vertex/index data");
    const uint64_t vertexBytes=uint64_t(vertexCount)*stride;
    const uint64_t indexBytes=uint64_t(indexCount)*sizeof(uint32_t);
    if(vertexBytes>kInputBytes||indexBytes>kInputBytes-vertexBytes)
        return fail(error,"mesh submission input exceeds 256 MiB");
    if(!mesh.hasAttribute(MeshAttribute::Position))
        return fail(error,"mesh submission requires float3 positions");
    const auto position=mesh.getAttributeInfo(MeshAttribute::Position);
    if(position.count<3||uint32_t(position.offset)+sizeof(float)*3>stride)
        return fail(error,"mesh submission position exceeds vertex stride");
    const uint32_t sectionCount=mesh.getSubmeshCount(),slotCount=mesh.getMaterialSlotCount();
    uint64_t metadataBytes=uint64_t(sectionCount)*sizeof(MeshSubmissionSection)
        +uint64_t(slotCount)*sizeof(std::string);
    if(metadataBytes>kMetadataBytes||slotCount>kMaterialSlots)
        return fail(error,"mesh submission metadata exceeds its count/byte budget");
    const auto* sections=mesh.getSubmeshes();
    if(sectionCount&&!sections)return fail(error,"mesh submission sections are missing");
    const uint32_t paletteCount=mesh.getSkinPaletteCount();
    const auto* palettes=mesh.getSkinPalettes();
    const uint32_t jointCount=mesh.getSkinPaletteJointCount();
    const auto* joints=mesh.getSkinPaletteJoints();
    if((paletteCount&&paletteCount!=sectionCount)||(paletteCount&&!palettes)||(jointCount&&!joints))
        return fail(error,"mesh submission palette table is inconsistent");
    if(uint64_t(jointCount)*sizeof(uint32_t)>kMetadataBytes)
        return fail(error,"mesh submission palette input exceeds budget");
    uint64_t visits=0;
    for(uint32_t i=0;i<sectionCount;++i) {
        if(uint64_t(sections[i].indexOffset)+sections[i].indexCount>indexCount)
            return fail(error,"mesh submission section index range is invalid");
        visits+=sections[i].indexCount;
        if(visits>kCenterIndexVisits)return fail(error,"mesh submission center work exceeds budget");
        if(paletteCount) {
            const auto& palette=palettes[i];
            if(uint64_t(palette.jointOffset)+palette.jointCount>jointCount)
                return fail(error,"mesh submission palette range is invalid");
            metadataBytes+=uint64_t(palette.jointCount)*sizeof(uint32_t);
            if(metadataBytes>kMetadataBytes)return fail(error,"mesh submission palettes exceed metadata budget");
        }
    }
    // Validate every GPU index before the existing uint16 narrowing path.
    // Position reads are within the declared stride and use memcpy for alignment.
    for(uint32_t i=0;i<indexCount;++i) {
        const auto vertex=mesh.getIndexData()[i];
        if(vertex>=vertexCount)return fail(error,"mesh submission index exceeds vertex count");
        float xyz[3];
        std::memcpy(xyz,mesh.getVertexData()+size_t(vertex)*stride+position.offset,sizeof(xyz));
        if(!std::isfinite(xyz[0])||!std::isfinite(xyz[1])||!std::isfinite(xyz[2]))
            return fail(error,"mesh submission position is nonfinite");
    }
    MeshSubmissionInfo prepared;
    prepared.vertexCount=vertexCount;prepared.indexCount=indexCount;
    prepared.materialSlots.reserve(slotCount);
    for(uint32_t i=0;i<slotCount;++i) {
        const char* slot=mesh.getMaterialSlot(i);
        const std::string_view value=slot?std::string_view(slot):std::string_view{};
        if(value.size()>kSlotStringBytes)
            return fail(error,"mesh submission material paths exceed metadata budget");
        metadataBytes+=value.size();
        if(metadataBytes>kMetadataBytes)
            return fail(error,"mesh submission material paths exceed metadata budget");
        prepared.materialSlots.emplace_back(value);
    }
    prepared.sections.reserve(sectionCount);
    const auto fallback=mesh.hasBounds()&&finite(mesh.getBounds().center)
        ?mesh.getBounds().center:ayt::math::FVector3{};
    for(uint32_t i=0;i<sectionCount;++i) {
        const auto& source=sections[i];
        MeshSubmissionSection section;
        section.firstIndex=source.indexOffset;section.indexCount=source.indexCount;
        section.materialIndex=source.materialIndex;section.vertexOffset=source.vertexOffset;
        section.localCenter=fallback;
        double sum[3]{};
        for(uint64_t cursor=source.indexOffset;cursor<uint64_t(source.indexOffset)+source.indexCount;++cursor) {
            float xyz[3];
            const auto vertex=mesh.getIndexData()[cursor];
            std::memcpy(xyz,mesh.getVertexData()+size_t(vertex)*stride+position.offset,sizeof(xyz));
            for(unsigned channel=0;channel<3;++channel)sum[channel]+=xyz[channel];
        }
        if(source.indexCount) section.localCenter={float(sum[0]/source.indexCount),
            float(sum[1]/source.indexCount),float(sum[2]/source.indexCount)};
        if(paletteCount&&palettes[i].jointCount) {
            const auto& palette=palettes[i];
            section.bonePalette.assign(joints+palette.jointOffset,joints+palette.jointOffset+palette.jointCount);
        }
        prepared.sections.push_back(std::move(section));
    }
    output=std::move(prepared);
    if(error)error->clear();
    return true;
}
} // namespace ayt::render::detail
