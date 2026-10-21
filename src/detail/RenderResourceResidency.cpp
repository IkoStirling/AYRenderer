#include "RenderResourceManager.h"
#include "TextureStorageAccounting.h"
#include <algorithm>
#include <array>
#include <queue>
#include <unordered_map>

namespace ayt::render::detail {
namespace {
ObservedTextureFilter filter(uint64_t flags,uint64_t mask,uint64_t point,uint64_t anisotropic) {
    const auto value=flags&mask;
    if(!value)return ObservedTextureFilter::Linear;
    if(value==point)return ObservedTextureFilter::Point;
    if(anisotropic&&value==anisotropic)return ObservedTextureFilter::AnisotropicDeviceMaximum;
    return ObservedTextureFilter::Unknown;
}
ObservedTextureAddress address(uint64_t flags,unsigned shift) {
    const auto value=(flags>>shift)&(BGFX_SAMPLER_U_MASK>>BGFX_SAMPLER_U_SHIFT);
    if(!value)return ObservedTextureAddress::Repeat;
    if(value==(BGFX_SAMPLER_U_MIRROR>>BGFX_SAMPLER_U_SHIFT))return ObservedTextureAddress::Mirror;
    if(value==(BGFX_SAMPLER_U_CLAMP>>BGFX_SAMPLER_U_SHIFT))return ObservedTextureAddress::Clamp;
    if(value==(BGFX_SAMPLER_U_BORDER>>BGFX_SAMPLER_U_SHIFT))return ObservedTextureAddress::Border;
    return ObservedTextureAddress::Unknown;
}
bool storageBytes(const GpuTexture& texture,uint64_t& bytes) {
    return texture.storageDescriptionKnown&&textureLogicalStorageBytes(texture.width,texture.height,
        texture.mipCount,texture.cube,texture.storageFormat,bytes);
}
TextureResidencyRow rowFor(uint64_t id,const GpuTexture& texture) {
    TextureResidencyRow row;
    row.textureId=id;row.width=texture.width;row.height=texture.height;
    row.borrowed=texture.borrowed;row.dynamic=texture.dynamic;
    row.storageDescriptionKnown=texture.storageDescriptionKnown;
    row.creationUploadBytesKnown=texture.creationUploadBytesKnown;
    if(row.creationUploadBytesKnown)row.creationUploadBytes=texture.uploadBytes;
    row.retainedCpuImageBytes=texture.cpuRgba8.size();
    row.sourcePayloadKnown=texture.sourcePayloadKnown;
    if(row.sourcePayloadKnown){
        row.sourceFormat=texture.sourceFormatName;row.sourceMipCount=texture.sourceMipCount;
        row.sourcePayloadBytes=texture.sourcePayloadBytes;
    }
    row.partialChainFallback=texture.partialChainFallback;row.samplerFallback=texture.samplerFallback;
    if(row.storageDescriptionKnown){
        row.kind=texture.cube?ObservedTextureKind::Cube:ObservedTextureKind::Texture2D;
        row.storageFormat=textureStorageFormatName(texture.storageFormat);row.mipCount=texture.mipCount;
        row.srgbKnown=true;row.srgb=texture.srgb;
        row.logicalStorageBytesKnown=storageBytes(texture,row.logicalStorageBytes);
        row.minFilter=filter(texture.samplerFlags,BGFX_SAMPLER_MIN_MASK,BGFX_SAMPLER_MIN_POINT,BGFX_SAMPLER_MIN_ANISOTROPIC);
        row.magFilter=filter(texture.samplerFlags,BGFX_SAMPLER_MAG_MASK,BGFX_SAMPLER_MAG_POINT,BGFX_SAMPLER_MAG_ANISOTROPIC);
        row.mipFilter=filter(texture.samplerFlags,BGFX_SAMPLER_MIP_MASK,BGFX_SAMPLER_MIP_POINT,0);
        row.addressU=address(texture.samplerFlags,BGFX_SAMPLER_U_SHIFT);
        row.addressV=address(texture.samplerFlags,BGFX_SAMPLER_V_SHIFT);
        row.addressW=address(texture.samplerFlags,BGFX_SAMPLER_W_SHIFT);
    }
    return row;
}
std::string boundedKey(const std::string& key) {
    if(key.size()<=kResourceResidencyMaxKeyBytes)return key;
    size_t size=kResourceResidencyMaxKeyBytes;
    // A valid UTF-8 key remains valid when its last code point crosses the cap.
    while(size&&(static_cast<unsigned char>(key[size])&0xc0u)==0x80u)--size;
    return key.substr(0,size);
}
struct KeyCandidates {
    std::array<const std::string*,kResourceResidencyMaxKeysPerTexture> keys{};
    size_t count=0;
    void consider(const std::string& key){
        if(count<keys.size())keys[count++]=&key;
        else if(key<*keys[count-1])keys[count-1]=&key;
        else return;
        std::sort(keys.begin(),keys.begin()+count,[](const auto* a,const auto* b){return *a<*b;});
    }
};
}

ResourceResidencySnapshot RenderResourceManager::resourceResidencySnapshot(uint64_t contextId,
    uint32_t maxTextureRows) const
{
    ResourceResidencySnapshot result;
    if(!_adapter.isInitialized())return result;
    result.available=true;result.contextId=contextId;
    result.meshEntries=_meshes.size();result.materialEntries=_materials.size();
    result.textureEntries=_textures.size();result.textureCacheKeys=_textureCacheByKey.size();
    const auto limit=std::min(maxTextureRows,kResourceResidencyMaxTextureRows);
    // Keep only the lowest K ids; scratch and returned rows remain bounded even
    // when the live cache contains far more textures. Totals still cover all.
    std::priority_queue<uint64_t> selected;
    for(const auto& [id,texture]:_textures){
        result.retainedCpuImageBytes+=texture.cpuRgba8.size();
        if(texture.borrowed)++result.borrowedTextures;
        else {
            ++result.ownedTextures;uint64_t bytes=0;
            if(storageBytes(texture,bytes))result.knownOwnedLogicalTextureBytes+=bytes;
            else ++result.unknownOwnedStorageTextures;
            if(texture.creationUploadBytesKnown)result.knownOwnedCreationUploadBytes+=texture.uploadBytes;
            else ++result.unknownOwnedUploadTextures;
        }
        if(limit){
            if(selected.size()<limit)selected.push(id);
            else if(id<selected.top()){selected.pop();selected.push(id);}
        }
    }
    std::vector<uint64_t> ids;ids.reserve(selected.size());
    while(!selected.empty()){ids.push_back(selected.top());selected.pop();}
    std::sort(ids.begin(),ids.end());result.textures.reserve(ids.size());
    std::unordered_map<uint64_t,size_t> indices;indices.reserve(ids.size());
    for(const auto id:ids){indices.emplace(id,result.textures.size());result.textures.push_back(rowFor(id,_textures.at(id)));}
    result.textureRowsOmitted=result.textureEntries-result.textures.size();
    std::vector<KeyCandidates> candidates(result.textures.size());
    if(!indices.empty())for(const auto& [key,id]:_textureCacheByKey){
        const auto found=indices.find(id);if(found==indices.end())continue;
        auto& row=result.textures[found->second];++row.cacheKeyCount;candidates[found->second].consider(key);
    }
    for(size_t index=0;index<result.textures.size();++index){
        auto& row=result.textures[index];const auto& keys=candidates[index];
        row.cacheKeys.reserve(keys.count);row.cacheKeysTruncated=row.cacheKeyCount>keys.count;
        for(size_t key=0;key<keys.count;++key){
            row.cacheKeysTruncated|=keys.keys[key]->size()>kResourceResidencyMaxKeyBytes;
            row.cacheKeys.push_back(boundedKey(*keys.keys[key]));
        }
    }
    return result;
}
} // namespace ayt::render::detail
