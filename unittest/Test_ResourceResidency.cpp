#include <AYTest.h>
#include <AYRenderer.h>
#include <AYRenderer/ResourceResidency.h>
#include <AYResource/assetsImpl/Texture.h>
#include "detail/RenderResourceManager.h"
#include "detail/TextureUploadPlan.h"
#include <algorithm>

using namespace ayt::render;
using namespace ayt::render::detail;
namespace {
struct ResidencyFixture {
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderResourceManager resources{adapter,pool};
    bool ready=false;
    ResidencyFixture(){BGFXInitParams init;init.backend=Backend::Noop;ready=adapter.initialize(init);}
    ~ResidencyFixture(){resources.shutdown();pool.shutdown();adapter.shutdown();}
};
const TextureResidencyRow* find(const ResourceResidencySnapshot& snapshot,uint64_t id){
    for(const auto& row:snapshot.textures)if(row.textureId==id)return &row;
    return nullptr;
}
}

TEST_SUITE(ResourceResidencyTests)
TEST_CASE(mesh_payload_totals_follow_creation_and_destruction)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    const float vertices[]={0,0,0, 1,0,0, 0,1,0};
    const uint16_t indices16[]={0,1,2};
    const uint32_t indices32[]={0,1,2};
    const auto layout=VertexLayoutDesc::position3();
    auto plain=f.resources.createMesh(vertices,3,layout,indices16,3);
    auto lines=f.resources.createMesh32(vertices,3,layout,indices32,3,true);
    CHECK(plain.isValid());CHECK(lines.isValid());
    if(!plain.isValid()||!lines.isValid())return;
    const auto& plainMesh=f.resources.meshes().at(plain.id);
    const auto& lineMesh=f.resources.meshes().at(lines.id);
    const uint64_t expectedPlainLines=bgfx::isValid(plainMesh.wireframeIndexBuffer)
        ? uint64_t(plainMesh.wireframeIndexCount)*sizeof(uint16_t) : 0;
    const uint64_t expectedLines=bgfx::isValid(lineMesh.wireframeIndexBuffer)
        ? uint64_t(lineMesh.wireframeIndexCount)*sizeof(uint32_t) : 0;
    const auto observed=f.resources.resourceResidencySnapshot(71,0);
    CHECK(observed.meshEntries==2);
    CHECK(observed.knownOwnedLogicalMeshBytes==36+6+expectedPlainLines+36+12+expectedLines);
    f.resources.destroyMesh(plain);
    CHECK(f.resources.resourceResidencySnapshot(71,0).knownOwnedLogicalMeshBytes==48+expectedLines);
    f.resources.destroyMesh(lines);
    CHECK(f.resources.resourceResidencySnapshot(71,0).knownOwnedLogicalMeshBytes==0);
}
TEST_CASE(public_observation_is_unavailable_before_initialization_and_owned_after_destroy)
{
    Renderer renderer;CHECK_FALSE(renderer.resourceResidencySnapshot().available);
    InitDesc init;init.backend=Backend::Noop;CHECK(renderer.initialize(init));
    const uint8_t pixels[16]={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
    auto texture=renderer.createTextureFromRgba8(2,2,pixels,"observed raw");CHECK(texture.isValid());
    const auto snapshot=renderer.resourceResidencySnapshot();CHECK(snapshot.available);
    CHECK(snapshot.contextId==renderer.resourceContextId());CHECK(snapshot.textureEntries==1);
    CHECK(snapshot.knownOwnedLogicalTextureBytes==16);CHECK(snapshot.knownOwnedCreationUploadBytes==16);
    const auto* row=find(snapshot,texture.id);CHECK(row);
    if(row){CHECK(row->storageFormat=="RGBA8");CHECK(row->mipCount==1);CHECK(row->logicalStorageBytes==16);
        CHECK(row->sourcePayloadKnown);CHECK(row->sourcePayloadBytes==16);CHECK(row->cacheKeys==std::vector<std::string>{"observed raw"});}
    renderer.destroyTexture(texture);CHECK(renderer.resourceResidencySnapshot().textureEntries==0);
    renderer.shutdown();CHECK_FALSE(renderer.resourceResidencySnapshot().available);
    CHECK(snapshot.textures.size()==1);CHECK(snapshot.textures[0].cacheKeys[0]=="observed raw");
    CHECK(renderer.initialize(init));
    const auto restarted=renderer.resourceResidencySnapshot(0);CHECK(restarted.available);
    CHECK(restarted.contextId!=snapshot.contextId);CHECK(restarted.textureEntries==0);
    CHECK(snapshot.contextId!=0);CHECK(snapshot.knownOwnedLogicalTextureBytes==16);
}
TEST_CASE(dynamic_and_borrowed_entries_do_not_invent_uploads_or_duplicate_owned_storage)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    const uint8_t pixels[64]={};auto owned=f.resources.createTextureFromRgba8(4,4,pixels,"owned");
    auto dynamic=f.resources.createDynamicTextureRgba8(2,3);CHECK(owned.isValid());CHECK(dynamic.isValid());
    if(!owned.isValid()||!dynamic.isValid())return;
    auto borrowed=f.resources.registerBorrowedTexture(f.resources.textures().at(owned.id).handle,4,4);
    CHECK(borrowed.isValid());
    const auto snapshot=f.resources.resourceResidencySnapshot(71,128);
    CHECK(snapshot.textureEntries==3);CHECK(snapshot.ownedTextures==2);CHECK(snapshot.borrowedTextures==1);
    CHECK(snapshot.knownOwnedLogicalTextureBytes==64+24);CHECK(snapshot.knownOwnedCreationUploadBytes==64);
    CHECK(snapshot.unknownOwnedStorageTextures==0);CHECK(snapshot.retainedCpuImageBytes==0);
    const auto* dyn=find(snapshot,dynamic.id);const auto* alias=find(snapshot,borrowed.id);CHECK(dyn);CHECK(alias);
    if(dyn){CHECK(dyn->dynamic);CHECK(dyn->logicalStorageBytes==24);CHECK(dyn->creationUploadBytesKnown);
        CHECK(dyn->creationUploadBytes==0);CHECK_FALSE(dyn->sourcePayloadKnown);}
    if(alias){CHECK(alias->borrowed);CHECK_FALSE(alias->storageDescriptionKnown);CHECK_FALSE(alias->logicalStorageBytesKnown);
        CHECK_FALSE(alias->srgbKnown);CHECK(alias->mipCount==0);CHECK(alias->kind==ObservedTextureKind::Unknown);}
    f.resources.destroyTexture(borrowed);CHECK(f.resources.textures().contains(owned.id));
}
TEST_CASE(expansion_and_partial_chain_observe_provided_payload_and_successful_gpu_shape)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    ayt::resource::Texture source;source._width=5;source._height=3;source._format=ayt::resource::TextureFormat::BC1;
    std::vector<uint8_t> blocks(32,0);for(size_t index=0;index<blocks.size();index+=8){blocks[index]=0;blocks[index+1]=0xf8;}
    source.setImageData(std::vector<uint8_t>(blocks));source.setMipmapLayout({0,16,24},{16,8,8});
    const auto plan=prepareTextureUpload(source,false);CHECK(plan.isValid());
    const auto texture=f.resources.createTextureFromPlan(plan,"expanded BC1");CHECK(texture.isValid());
    auto snapshot=f.resources.resourceResidencySnapshot(71,128);const auto* row=find(snapshot,texture.id);CHECK(row);
    if(row){CHECK(row->storageFormat=="RGBA8");CHECK(row->sourceFormat=="BC1");CHECK(row->sourcePayloadBytes==32);
        CHECK(row->sourceMipCount==3);CHECK(row->mipCount==3);CHECK(row->logicalStorageBytes==72);CHECK(row->creationUploadBytes==72);}
    source.setImageData(std::vector<uint8_t>(blocks.begin(),blocks.begin()+24));source.setMipmapLayout({0,16},{16,8});
    const auto prefix=f.resources.createTextureFromPlan(prepareTextureUpload(source,false),"BC1 prefix");CHECK(prefix.isValid());
    snapshot=f.resources.resourceResidencySnapshot(71,128);row=find(snapshot,prefix.id);CHECK(row);
    if(row){CHECK(row->partialChainFallback);CHECK(row->sourceMipCount==2);CHECK(row->mipCount==1);
        CHECK(row->sourcePayloadBytes==24);CHECK(row->logicalStorageBytes==60);CHECK(row->creationUploadBytes==60);}
}
TEST_CASE(row_limits_keep_complete_totals_and_keys_do_not_exceed_the_utf8_budget)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    const uint8_t pixel[4]={255,127,63,255};std::vector<uint64_t> ids;
    std::string key(1023,'x');key+="\xe7\x95\x8c";
    auto first=f.resources.createTextureFromRgba8(1,1,pixel,key);CHECK(first.isValid());ids.push_back(first.id);
    for(unsigned index=0;index<514;++index){const auto handle=f.resources.createTextureFromRgba8(1,1,pixel,"row-"+std::to_string(index));
        CHECK(handle.isValid());ids.push_back(handle.id);}
    const auto totals=f.resources.resourceResidencySnapshot(71,0);CHECK(totals.textures.empty());
    CHECK(totals.textureEntries==515);CHECK(totals.textureRowsOmitted==515);CHECK(totals.knownOwnedLogicalTextureBytes==515*4);
    const auto snapshot=f.resources.resourceResidencySnapshot(71,UINT32_MAX);
    CHECK(snapshot.textures.size()==512);CHECK(snapshot.textureRowsOmitted==3);
    CHECK(snapshot.textureEntries==totals.textureEntries);CHECK(snapshot.knownOwnedLogicalTextureBytes==totals.knownOwnedLogicalTextureBytes);
    for(size_t index=0;index<snapshot.textures.size();++index)CHECK(snapshot.textures[index].textureId==ids[index]);
    const auto* row=find(snapshot,first.id);CHECK(row);if(row){CHECK(row->cacheKeysTruncated);CHECK(row->cacheKeyCount==1);
        CHECK(row->cacheKeys.size()==1);if(!row->cacheKeys.empty())CHECK(row->cacheKeys[0]==std::string(1023,'x'));}
}
TEST_CASE(raw_formats_and_dynamic_updates_preserve_separate_accounting)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    const uint8_t r8[6]={1,2,3,4,5,6};
    auto raw=f.resources.createTextureFromData(3,2,uint32_t(bgfx::TextureFormat::R8),r8,sizeof(r8),"raw R8");
    const uint8_t pixels[16]={};
    const auto dynamic=f.resources.createDynamicTextureRgba8(2,2);
    CHECK(raw.isValid());CHECK(dynamic.isValid());
    if(!raw.isValid()||!dynamic.isValid())return;
    const auto before=f.resources.resourceResidencySnapshot(71,128);
    CHECK(before.knownOwnedLogicalTextureBytes==6+16);CHECK(before.knownOwnedCreationUploadBytes==6);
    CHECK(before.retainedCpuImageBytes==0);
    const auto* rawRow=find(before,raw.id);CHECK(rawRow);
    if(rawRow){CHECK(rawRow->storageFormat=="R8");CHECK(rawRow->logicalStorageBytes==6);
        CHECK(rawRow->sourceFormat=="R8");CHECK(rawRow->sourcePayloadBytes==6);}
    CHECK(f.resources.updateTextureFromRgba8(dynamic,pixels));
    CHECK(f.resources.updateTextureFromRgba8(dynamic,pixels));
    const auto updated=f.resources.resourceResidencySnapshot(71,128);
    CHECK(updated.knownOwnedCreationUploadBytes==before.knownOwnedCreationUploadBytes);
    CHECK(updated.knownOwnedLogicalTextureBytes==before.knownOwnedLogicalTextureBytes);
    f.resources.destroyTexture(raw);CHECK(f.resources.resourceResidencySnapshot(71,0).knownOwnedLogicalTextureBytes==16);
    CHECK(before.knownOwnedLogicalTextureBytes==22);
}
TEST_CASE(observed_sampler_defaults_match_the_uploaded_flags_after_policy_fallback)
{
    ResidencyFixture f;CHECK(f.ready);if(!f.ready)return;
    TextureUploadPlan plan;plan.width=plan.height=2;plan.mipCount=1;
    plan.format=bgfx::TextureFormat::RGBA8;plan.bytes.resize(16);
    plan.flags=BGFX_TEXTURE_SRGB|BGFX_SAMPLER_MIN_POINT|BGFX_SAMPLER_MAG_POINT|BGFX_SAMPLER_MIP_POINT
        |BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_MIRROR|BGFX_SAMPLER_W_BORDER;
    plan.samplerFallback=true;
    const auto texture=f.resources.createTextureFromPlan(plan,"creation defaults");CHECK(texture.isValid());
    const auto snapshot=f.resources.resourceResidencySnapshot(71,128);const auto* row=find(snapshot,texture.id);CHECK(row);
    if(row){CHECK(row->srgbKnown);CHECK(row->srgb);CHECK(row->samplerFallback);
        CHECK(row->minFilter==ObservedTextureFilter::Point);CHECK(row->magFilter==ObservedTextureFilter::Point);
        CHECK(row->mipFilter==ObservedTextureFilter::Point);CHECK(row->addressU==ObservedTextureAddress::Clamp);
        CHECK(row->addressV==ObservedTextureAddress::Mirror);CHECK(row->addressW==ObservedTextureAddress::Border);
        CHECK_FALSE(row->sourcePayloadKnown);}
    plan.flags=BGFX_SAMPLER_MIN_ANISOTROPIC;
    const auto af=f.resources.createTextureFromPlan(plan,"anisotropic defaults");CHECK(af.isValid());
    const auto afSnapshot=f.resources.resourceResidencySnapshot(71,128);row=find(afSnapshot,af.id);CHECK(row);
    if(row){CHECK(row->minFilter==ObservedTextureFilter::AnisotropicDeviceMaximum);
        CHECK(row->magFilter==ObservedTextureFilter::Linear);CHECK(row->mipFilter==ObservedTextureFilter::Linear);
        CHECK(row->addressU==ObservedTextureAddress::Repeat);CHECK_FALSE(row->srgb);}
}
TEST_SUITE_END
