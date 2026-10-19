#include "detail/GpuPointState.h"
#include "detail/BGFXAdapter.h"
#include "detail/BgfxMatrix.h"
#include "detail/GpuPointShaderBinary.h"
#if defined(AY_RENDERER_POINT_GPU)
#include "GpuPointBinaries.h"
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>
namespace ayt::render::detail {
namespace {
template<class H> void destroyGpu(H& handle) noexcept {if(bgfx::isValid(handle))bgfx::destroy(handle);handle=BGFX_INVALID_HANDLE;}
struct ReadbackPacket {std::array<float,4> data;bgfx::TextureHandle texture=BGFX_INVALID_HANDLE,counter=BGFX_INVALID_HANDLE;uint32_t ready=UINT32_MAX,computeFrame=0,blitFrame=UINT32_MAX,source=0;uint64_t serial=0;bool cancelled=false;};
struct PointBuffers {
    bgfx::DynamicVertexBufferHandle source=BGFX_INVALID_HANDLE,instances=BGFX_INVALID_HANDLE;
    bgfx::DynamicIndexBufferHandle flags=BGFX_INVALID_HANDLE,counts=BGFX_INVALID_HANDLE,offsets=BGFX_INVALID_HANDLE;
    bgfx::IndirectBufferHandle indirect=BGFX_INVALID_HANDLE;uint32_t capacity=0;
    void destroy() noexcept {destroyGpu(source);destroyGpu(instances);destroyGpu(flags);destroyGpu(counts);destroyGpu(offsets);destroyGpu(indirect);capacity=0;}
};
struct SourcePoint {float value[8];};
static_assert(sizeof(SourcePoint)==32);
}
struct GpuPointContext::Impl {
    BGFXAdapter* adapter=nullptr;std::shared_ptr<std::atomic<bool>> token;bool deviceLive=true;
    bgfx::ProgramHandle mark=BGFX_INVALID_HANDLE,count=BGFX_INVALID_HANDLE,prefix=BGFX_INVALID_HANDLE,compact=BGFX_INVALID_HANDLE,draw=BGFX_INVALID_HANDLE;
    bgfx::UniformHandle params=BGFX_INVALID_HANDLE,vp=BGFX_INVALID_HANDLE,sampler=BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle quad=BGFX_INVALID_HANDLE;bgfx::IndexBufferHandle indices=BGFX_INVALID_HANDLE;
    bgfx::TextureHandle disc=BGFX_INVALID_HANDLE,dummyCount=BGFX_INVALID_HANDLE;
    std::vector<std::weak_ptr<GpuPointState>> states;
    std::vector<std::shared_ptr<ReadbackPacket>> packets;
};
struct GpuPointState::Impl {
    std::shared_ptr<GpuPointContext> context;PointBuffers buffers;std::vector<SourcePoint> upload;
    bgfx::TextureHandle counter=BGFX_INVALID_HANDLE,readback=BGFX_INVALID_HANDLE;
    std::shared_ptr<ReadbackPacket> pending;uint64_t serial=0;uint32_t source=0;
    bool sampleValid=false;uint64_t sampleSerial=0;uint32_t sampleSource=0,sampleVisible=0;
};
GpuPointContext::GpuPointContext():_impl(std::make_unique<Impl>()){}
GpuPointContext::~GpuPointContext(){releaseBeforeDeviceShutdown();}
bool GpuPointContext::alive() const noexcept {return _impl->deviceLive&&_impl->token&&_impl->token->load(std::memory_order_acquire);}
std::shared_ptr<GpuPointContext> GpuPointContext::create(BGFXAdapter& adapter,std::shared_ptr<std::atomic<bool>> alive,std::string* error){
    const auto fail=[&](const char* text)->std::shared_ptr<GpuPointContext>{if(error)*error=text;return {};};
#if !defined(AY_RENDERER_POINT_GPU)
    return fail("Point GPU kernels were not built for this target");
#else
    const auto* caps=bgfx::getCaps();const auto backend=bgfx::getRendererType();
    const uint64_t required=BGFX_CAPS_COMPUTE|BGFX_CAPS_DRAW_INDIRECT|BGFX_CAPS_INSTANCING;
    if(!adapter.isInitialized()||!caps||(backend!=bgfx::RendererType::Direct3D11&&backend!=bgfx::RendererType::Direct3D12)
        ||(caps->supported&required)!=required||caps->limits.maxComputeBindings<5
        ||!(caps->formats[bgfx::TextureFormat::RGBA32F]&BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE))return fail("Point GPU culling requires D3D11/12 compute, indirect and instancing");
    auto result=std::shared_ptr<GpuPointContext>(new GpuPointContext());auto& i=*result->_impl;i.adapter=&adapter;i.token=std::move(alive);
    const auto shader=[&](std::span<const uint8_t> bytes,const char* stage,uint64_t hash){
        if(!pointgpu::validBinary(bytes,stage,hash))return bgfx::ShaderHandle BGFX_INVALID_HANDLE;
        return bgfx::createShader(bgfx::copy(bytes.data(),uint32_t(bytes.size())));
    };
    const auto compute=[&](std::span<const uint8_t> bytes,uint64_t hash){auto code=shader(bytes,"CSH",hash);return bgfx::isValid(code)?bgfx::createProgram(code,true):bgfx::ProgramHandle BGFX_INVALID_HANDLE;};
    i.mark=compute(pointgpu::baked::mark,pointgpu::baked::markHash);i.count=compute(pointgpu::baked::count,pointgpu::baked::countHash);
    i.prefix=compute(pointgpu::baked::prefix,pointgpu::baked::prefixHash);i.compact=compute(pointgpu::baked::compact,pointgpu::baked::compactHash);
    auto vertex=shader(pointgpu::baked::vertex,"VSH",pointgpu::baked::vertexHash),fragment=shader(pointgpu::baked::fragment,"FSH",pointgpu::baked::fragmentHash);
    if(bgfx::isValid(vertex)&&bgfx::isValid(fragment))i.draw=bgfx::createProgram(vertex,fragment,true);
    else {destroyGpu(vertex);destroyGpu(fragment);}
    i.params=bgfx::createUniform("u_epParams",bgfx::UniformType::Vec4,4);i.vp=bgfx::createUniform("u_epViewProjection",bgfx::UniformType::Mat4);
    i.sampler=bgfx::createUniform("s_epDisc",bgfx::UniformType::Sampler);
    bgfx::VertexLayout layout;layout.begin().add(bgfx::Attrib::Position,3,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0,2,bgfx::AttribType::Float).end();
    const float quad[20]={-.5f,-.5f,0,0,0,.5f,-.5f,0,1,0,.5f,.5f,0,1,1,-.5f,.5f,0,0,1};const uint16_t indices[6]={0,1,2,0,2,3};
    i.quad=bgfx::createVertexBuffer(bgfx::copy(quad,sizeof(quad)),layout);i.indices=bgfx::createIndexBuffer(bgfx::copy(indices,sizeof(indices)));
    std::array<uint8_t,32*32*4> pixels;
    for(int y=0;y<32;++y)for(int x=0;x<32;++x){const float dx=(x+.5f-16)/16,dy=(y+.5f-16)/16;const auto index=(y*32+x)*4;
        const float falloff=std::clamp(1-dx*dx-dy*dy,0.f,1.f);pixels[index]=pixels[index+1]=pixels[index+2]=255;pixels[index+3]=uint8_t(255*falloff*falloff);}
    i.disc=bgfx::createTexture2D(32,32,false,1,bgfx::TextureFormat::RGBA8,BGFX_SAMPLER_U_CLAMP|BGFX_SAMPLER_V_CLAMP,bgfx::copy(pixels.data(),uint32_t(pixels.size())));
    i.dummyCount=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_COMPUTE_WRITE);
    if(!bgfx::isValid(i.mark)||!bgfx::isValid(i.count)||!bgfx::isValid(i.prefix)||!bgfx::isValid(i.compact)||!bgfx::isValid(i.draw)
        ||!bgfx::isValid(i.params)||!bgfx::isValid(i.vp)||!bgfx::isValid(i.sampler)||!bgfx::isValid(i.quad)||!bgfx::isValid(i.indices)||!bgfx::isValid(i.disc)||!bgfx::isValid(i.dummyCount))return fail("Point GPU shared resources unavailable");
    if(error)error->clear();return result;
#endif
}
std::shared_ptr<GpuPointState> GpuPointContext::createState(std::string* error){
    std::erase_if(_impl->states,[](const auto& state){return state.expired();});
    if(!alive()||_impl->states.size()>=8){if(error)*error="Point GPU cloud resource budget exceeded (8)";return {};}
    auto state=std::shared_ptr<GpuPointState>(new GpuPointState(shared_from_this()));_impl->states.push_back(state);return state;
}
void GpuPointContext::poll() noexcept {
    if(!_impl->deviceLive||!_impl->adapter||!_impl->adapter->isInitialized())return;
    const auto frame=_impl->adapter->gpuFrameCounter();
    for(auto& packet:_impl->packets)if(!packet->cancelled){
        if(packet->blitFrame==UINT32_MAX&&frame>packet->computeFrame){
            bgfx::blit(kPointReadbackView,packet->texture,0,0,packet->counter);packet->blitFrame=frame;
        }else if(packet->blitFrame!=UINT32_MAX&&packet->ready==UINT32_MAX&&frame>packet->blitFrame)
            packet->ready=bgfx::readTexture(packet->texture,packet->data.data());
    }
    std::erase_if(_impl->packets,[&](const auto& packet){return packet->cancelled||(packet->ready!=UINT32_MAX&&frame>packet->ready);});
}
void GpuPointContext::releaseBeforeDeviceShutdown() noexcept {
    auto& i=*_impl;if(!i.deviceLive)return;
    for(auto& weak:i.states)if(auto state=weak.lock())state->releaseGpu();i.states.clear();
    destroyGpu(i.mark);destroyGpu(i.count);destroyGpu(i.prefix);destroyGpu(i.compact);destroyGpu(i.draw);
    destroyGpu(i.params);destroyGpu(i.vp);destroyGpu(i.sampler);destroyGpu(i.quad);destroyGpu(i.indices);destroyGpu(i.disc);destroyGpu(i.dummyCount);
    i.deviceLive=false;
    // Keep pending host destinations alive until bgfx shutdown has drained.
}
void GpuPointContext::afterDeviceShutdown() noexcept {_impl->packets.clear();}
GpuPointState::GpuPointState(std::shared_ptr<GpuPointContext> context):_impl(std::make_unique<Impl>()){_impl->context=std::move(context);}
GpuPointState::~GpuPointState(){releaseGpu();}
bool GpuPointState::alive() const noexcept {return _impl->context&&_impl->context->alive();}
void GpuPointState::releaseGpu() noexcept {
    auto& i=*_impl;if(i.context->_impl->deviceLive){i.buffers.destroy();destroyGpu(i.counter);destroyGpu(i.readback);}if(i.pending&&i.pending->ready==UINT32_MAX)i.pending->cancelled=true;i.pending.reset();i.source=0;
}
bool GpuPointState::prepare(std::span<const EmissivePoint> points,const math::Float4x4& view,const math::Float4x4& projection,
    uint16_t width,uint16_t height,EmissivePointSettings settings,EmissivePointStats& stats,std::string* error){
    const auto fail=[&](const char* text){if(error)*error=text;return false;};
    if(!alive()||points.empty()||points.size()>65536||points.size()>settings.maxPoints||!width||!height)return fail("Invalid GPU point submission");
    auto& i=*_impl;auto& c=*i.context->_impl;const auto frame=c.adapter->gpuFrameCounter();i.context->poll();
    if(i.pending&&i.pending->ready!=UINT32_MAX&&frame>i.pending->ready){const auto& packet=*i.pending;const float count=packet.data[0];
        i.sampleValid=std::isfinite(count)&&count>=0&&count<=packet.source&&std::floor(count)==count&&packet.data[1]==packet.source&&packet.data[3]==1;
        if(i.sampleValid){i.sampleSerial=packet.serial;i.sampleSource=packet.source;i.sampleVisible=uint32_t(count);}i.pending.reset();}
    const uint32_t source=uint32_t(points.size()),groups=(source+255)/256;
    if(i.buffers.capacity<source){
        PointBuffers next;next.capacity=std::bit_ceil(std::max(source,256u));
        bgfx::VertexLayout input,output;input.begin().add(bgfx::Attrib::TexCoord0,4,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord1,4,bgfx::AttribType::Float).end();
        output.begin().add(bgfx::Attrib::TexCoord0,4,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord1,4,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord2,4,bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord3,4,bgfx::AttribType::Float).end();
        const uint16_t vectorFormat=BGFX_BUFFER_COMPUTE_FORMAT_32X4|BGFX_BUFFER_COMPUTE_TYPE_FLOAT;
        const uint16_t scalarFormat=BGFX_BUFFER_INDEX32|BGFX_BUFFER_COMPUTE_FORMAT_32X1|BGFX_BUFFER_COMPUTE_TYPE_UINT|BGFX_BUFFER_COMPUTE_READ_WRITE;
        next.source=bgfx::createDynamicVertexBuffer(next.capacity,input,vectorFormat|BGFX_BUFFER_COMPUTE_READ);
        next.instances=bgfx::createDynamicVertexBuffer(next.capacity,output,vectorFormat|BGFX_BUFFER_COMPUTE_READ_WRITE);
        next.flags=bgfx::createDynamicIndexBuffer(next.capacity,scalarFormat);next.counts=bgfx::createDynamicIndexBuffer(256,scalarFormat);next.offsets=bgfx::createDynamicIndexBuffer(256,scalarFormat);
        next.indirect=bgfx::createIndirectBuffer(1);
        if(!bgfx::isValid(next.source)||!bgfx::isValid(next.instances)||!bgfx::isValid(next.flags)||!bgfx::isValid(next.counts)||!bgfx::isValid(next.offsets)||!bgfx::isValid(next.indirect)){next.destroy();return fail("Point GPU buffers unavailable");}
        i.buffers.destroy();i.buffers=next;
    }
    const auto required=BGFX_CAPS_TEXTURE_READ_BACK|BGFX_CAPS_TEXTURE_BLIT;const bool wantDiagnostics=settings.gpuDiagnostics&&(bgfx::getCaps()->supported&required)==required;
    if(wantDiagnostics&&!bgfx::isValid(i.counter)){
        i.counter=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_COMPUTE_WRITE);
        i.readback=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
        if(!bgfx::isValid(i.counter)||!bgfx::isValid(i.readback)){destroyGpu(i.counter);destroyGpu(i.readback);return fail("Point GPU diagnostics unavailable");}
    }
    const bool capture=wantDiagnostics&&!i.pending;
    i.upload.resize(source);for(size_t index=0;index<points.size();++index){const auto& p=points[index];i.upload[index]={{p.position.x,p.position.y,p.position.z,p.radius,p.color.x,p.color.y,p.color.z,p.intensity}};}
    bgfx::update(i.buffers.source,0,bgfx::copy(i.upload.data(),uint32_t(i.upload.size()*sizeof(SourcePoint))));
    const auto inverse=view.inverse();const float scale=std::abs(projection(1,1))*height*.5f;
    const float parameters[16]={float(source),settings.minimumPixelRadius,scale,settings.preserveEnergy?1.f:0.f,std::abs(projection(0,0)),std::abs(projection(1,1)),float(groups),capture?1.f:0.f,
        inverse(0,0),inverse(1,0),inverse(2,0),0,inverse(0,1),inverse(1,1),inverse(2,1),0};
    float matrix[16];toBgfxColumnMajor(projection*view,matrix);
    const auto uniforms=[&](){bgfx::setUniform(c.params,parameters,4);bgfx::setUniform(c.vp,matrix);};
    bgfx::setViewName(kPointMarkView,"Point.Mark");bgfx::setViewName(kPointCountView,"Point.Count");bgfx::setViewName(kPointPrefixView,"Point.Prefix");bgfx::setViewName(kPointCompactView,"Point.Compact");bgfx::setViewName(kPointReadbackView,"Point.Readback");
    uniforms();bgfx::setBuffer(0,i.buffers.source,bgfx::Access::Read);bgfx::setBuffer(1,i.buffers.flags,bgfx::Access::Write);bgfx::dispatch(kPointMarkView,c.mark,(source+63)/64);
    uniforms();bgfx::setBuffer(1,i.buffers.flags,bgfx::Access::Read);bgfx::setBuffer(2,i.buffers.counts,bgfx::Access::Write);bgfx::dispatch(kPointCountView,c.count,(groups+63)/64);
    uniforms();bgfx::setBuffer(1,i.buffers.offsets,bgfx::Access::Write);bgfx::setBuffer(2,i.buffers.counts,bgfx::Access::Read);bgfx::setBuffer(3,i.buffers.indirect,bgfx::Access::Write);
    bgfx::setImage(4,capture?i.counter:c.dummyCount,0,bgfx::Access::Write,bgfx::TextureFormat::RGBA32F);bgfx::dispatch(kPointPrefixView,c.prefix,1);
    uniforms();bgfx::setBuffer(0,i.buffers.source,bgfx::Access::Read);bgfx::setBuffer(1,i.buffers.flags,bgfx::Access::Read);bgfx::setBuffer(2,i.buffers.offsets,bgfx::Access::Read);bgfx::setBuffer(3,i.buffers.instances,bgfx::Access::Write);bgfx::dispatch(kPointCompactView,c.compact,(source+63)/64);
    if(++i.serial==0)++i.serial;i.source=source;
    if(capture){auto packet=std::make_shared<ReadbackPacket>();packet->data.fill(std::numeric_limits<float>::quiet_NaN());packet->source=source;packet->serial=i.serial;
        packet->texture=i.readback;packet->counter=i.counter;packet->computeFrame=frame;i.pending=packet;c.packets.push_back(packet);
        // readTexture is a resource command: enqueue it only after the frame
        // containing this sorted blit, otherwise it snapshots the old texture.
    }
    EmissivePointStats next;next.source=source;next.batches=1;next.instanced=true;next.gpuCulling=true;next.countsExact=false;next.gpuSubmittedSerial=i.serial;
    next.gpuSampleValid=i.sampleValid;next.gpuSampleSerial=i.sampleSerial;next.gpuSampleSource=i.sampleSource;next.gpuSampleVisible=i.sampleVisible;stats=next;
    if(error)error->clear();return true;
}
void GpuPointState::submit(uint16_t view){
    if(!alive()||!_impl->source)return;auto& i=*_impl;auto& c=*i.context->_impl;
    static constexpr float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bgfx::setTransform(identity);bgfx::setVertexBuffer(0,c.quad);bgfx::setIndexBuffer(c.indices);bgfx::setInstanceDataBuffer(i.buffers.instances,0,i.source);bgfx::setTexture(0,c.sampler,c.disc);
    bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_DEPTH_TEST_LEQUAL|BGFX_STATE_MSAA|BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA,BGFX_STATE_BLEND_ONE,BGFX_STATE_BLEND_ZERO,BGFX_STATE_BLEND_ONE));
    bgfx::submit(view,c.draw,i.buffers.indirect,0,1);
}
} // namespace ayt::render::detail
