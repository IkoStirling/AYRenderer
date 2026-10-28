#include "detail/RenderViewNamespaces.h"
#include "detail/TextureImageLoader.h"
#include <AYRenderer.h>
#include <AYRenderer/TransparentFrameExport.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYTest.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace ayt::render;
namespace {
TEST_SUITE(CameraViews)
TEST_CASE(ui_and_preview_ranges_never_overlap_fixed_scene_or_runtime_camera_views) {
    using namespace ayt::render::detail;
    // Independent flat occupancy oracle, so adding main/internal Views does
    // not require changing UI budget literals or expected free-view numbers.
    std::array<bool,kRenderViewCapacity> occupied{};
    for(auto view:kRenderViewOrder)occupied[view]=true;
    for(size_t view=kCameraFirstView;view<=kCameraLastView;++view)occupied[view]=true;
    occupied[kCameraReadbackView]=true;
    const auto expectedRange=[&](uint16_t start,uint16_t count)->uint16_t {
        for(uint16_t candidate=start;uint32_t(candidate)+count<=243;++candidate)
            if(std::none_of(occupied.begin()+candidate,occupied.begin()+candidate+count,[](bool value){return value;}))return candidate;
        return UINT16_MAX;
    };
    CHECK(uiViewCapacity()==std::count(occupied.begin()+32,occupied.begin()+243,false));
    CHECK(nextUiViewRange(32,1)==expectedRange(32,1)); CHECK(nextUiViewRange(62,4)==expectedRange(62,4));
    CHECK(nextUiViewRange(64,1)==expectedRange(64,1)); CHECK(nextUiViewRange(242,2)==UINT16_MAX);
    for(uint16_t next=32; next<=242;) {
        next=nextUiViewRange(next,1); if(next==UINT16_MAX) break;
        CHECK(next<kCameraFirstView || next>kCameraLastView);
        CHECK(next!=kCameraReadbackView);
        for(auto reserved:kRenderViewOrder) CHECK(next!=reserved);
        ++next;
    }
}
TEST_CASE(layer_filter_preserves_2d_and_owned_payloads) {
    RenderScene source(RenderScene::DrawDataOwnership::OwnedCpu), visible;
    DrawItem first; first.mesh.id=1; first.cameraLayerMask=1; source.add(first);
    first.mesh.id=2; first.cameraLayerMask=4; source.add(first);
    DrawPayload2D payload; payload.tintRGBA.x=.4f;
    first.mesh.id=3; first.payload=&payload; source.add(first);
    source.setOverlayCamera2D(ayt::math::Float4x4::identity(),ayt::math::Float4x4::identity(),8);
    copyCameraVisibleScene(source,visible,4);
    source.clear(); payload.tintRGBA.x=0;
    CHECK(visible.items().size()==2); CHECK(visible.items()[0].mesh.id==2);
    CHECK(visible.items()[1].payload!=nullptr);
    CHECK_FLOAT_EQ(visible.items()[1].payload->tintRGBA.x,.4f,1e-5f);
    CHECK(visible.overlayCamera2D().layerMask==8);
}
TEST_CASE(region_filter_keeps_only_matching_3d_draws) {
    RenderScene source, visible;
    DrawItem item; item.mesh.id=1; item.spaceRegionId=1; source.add(item);
    item.mesh.id=2; item.spaceRegionId=2; source.add(item);
    item.mesh.id=3; item.spaceRegionId=0; source.add(item);
    copyCameraVisibleScene(source,visible,0xFFFFFFFFu,true,2);
    CHECK(visible.items().size()==1);
    CHECK(visible.items()[0].mesh.id==2);
    copyCameraVisibleScene(source,visible,0xFFFFFFFFu,true,0);
    CHECK(visible.items().size()==3);
}
TEST_CASE(view_batches_validate_before_submission_and_outputs_have_explicit_lifetime) {
    Renderer renderer; InitDesc desc; desc.backend=Backend::Noop; desc.width=320; desc.height=240;
    CHECK(renderer.initialize(desc));
    RenderScene scene;
    const auto output=renderer.createCameraOutput(64,64);
    CHECK(output.id!=0); CHECK(renderer.cameraOutputTexture(output).isValid());
    std::array<CameraRenderView,2> views;
    views[0].id=0; views[0].scene=&scene; views[0].width=160; views[0].height=240;
    views[1]=views[0]; views[1].id=1; views[1].x=160;
    renderer.beginFrame(); CHECK(renderer.renderViews(views)); renderer.endFrame();
    CHECK_FALSE(renderer.renderViews(views));
    renderer.beginFrame();
    views[1].id=0; CHECK_FALSE(renderer.renderViews(views));
    views[1].id=1; views[1].projection.row[0].x=std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(renderer.renderViews(views));
    views[1]=views[0]; views[1].id=1; views[1].output=output;
    views[1].width=64; views[1].height=64;
    views[1].width=63; CHECK_FALSE(renderer.renderViews(views)); views[1].width=64;
    CHECK(renderer.renderViews(views)); CHECK_FALSE(renderer.renderViews(views)); renderer.endFrame();
    renderer.beginFrame();
    renderer.destroyCameraOutput(output); CHECK_FALSE(renderer.cameraOutputTexture(output).isValid());
    CHECK_FALSE(renderer.renderViews(views)); renderer.destroyCameraOutput(output);
    DrawPayload2D payload; payload.renderDomain=RenderDomain2D::WorldLit;
    DrawItem item; item.payload=&payload; scene.add(item);
    views[1].output={}; CHECK_FALSE(renderer.renderViews(views));
    renderer.endFrame();
    renderer.shutdown();
}
TEST_CASE(transparent_2d_output_rejects_opaque_clear_and_3d_before_submission) {
    Renderer renderer; InitDesc desc; desc.backend=Backend::Noop;
    desc.width=64; desc.height=64;
    CHECK(renderer.initialize(desc));
    RenderScene scene;
    const auto output=renderer.createCameraOutput(32,32);
    CameraRenderView view;
    view.scene=&scene; view.width=32; view.height=32;
    view.transparent2DOutput=true;
    renderer.beginFrame();
    CHECK_FALSE(renderer.renderViews(std::array{view}));
    view.output=output;
    CHECK_FALSE(renderer.renderViews(std::array{view}));
    view.clearRgba=0;
    CHECK(renderer.renderViews(std::array{view}));
    renderer.endFrame();
    DrawItem item; item.mesh.id=10; scene.add(item);
    renderer.beginFrame();
    CHECK_FALSE(renderer.renderViews(std::array{view}));
    scene.clear();
    DrawPayload2D payload;
    payload.renderDomain=RenderDomain2D::WorldLit;
    item.payload=&payload; scene.add(item);
    CHECK_FALSE(renderer.renderViews(std::array{view}));
    payload.renderDomain=RenderDomain2D::SceneOverlay;
    CHECK(renderer.renderViews(std::array{view}));
    renderer.endFrame();
    renderer.destroyCameraOutput(output);
    renderer.shutdown();
}
TEST_CASE(portal_surface_material_binds_an_existing_camera_output) {
    Renderer renderer; InitDesc desc; desc.backend=Backend::Noop;
    desc.width=64; desc.height=64;
    CHECK(renderer.initialize(desc));
    CHECK_FALSE(renderer.createPortalSurfaceMaterial({999}).isValid());
    auto output=renderer.createCameraOutput(32,32);
    CHECK(output.id!=0);
    auto material=renderer.createPortalSurfaceMaterial(output);
    CHECK(material.isValid());
    auto quad=renderer.createUnitQuad(); CHECK(quad.isValid());
    RenderScene scene; DrawItem portal;
    portal.mesh=quad; portal.material=material; portal.spaceRegionId=1;
    scene.add(portal);
    std::array<CameraRenderView,2> views;
    views[0].id=1; views[0].scene=&scene; views[0].spaceRegionId=2;
    views[0].width=32; views[0].height=32; views[0].output=output;
    views[1]=views[0]; views[1].id=2; views[1].spaceRegionId=1;
    views[1].width=64; views[1].height=64; views[1].output={};
    renderer.beginFrame(); CHECK(renderer.renderViews(views)); renderer.endFrame();
    renderer.destroyMesh(quad);
    renderer.destroyMaterial(material);
    renderer.destroyCameraOutput(output);
    renderer.shutdown();
}
TEST_CASE(subsystem_extracts_each_view_with_matching_aspect_and_restores_primary) {
    int window=0; RendererSubSystem::setWindowProvider({});
    RendererSubSystem::setBootstrapBackend(Backend::Noop);
    RendererSubSystem::setBootstrapWindow(&window,320,240);
    RendererSubSystem::setBootstrapViewport(0,0,320,240);
    RendererSubSystem sub; CHECK(sub.initialize());
    const std::vector<CameraViewportDesc> recipes{{0,0,0,160,240,{}},{1,160,0,160,120,{}}};
    CHECK(sub.setCameraViewports(recipes));
    int owner=0; std::vector<int> observed; std::vector<float> aspects;
    sub.setSceneCameraProviderForOwner(&owner,[&](SceneCamera3D& camera) {
        CHECK_FALSE(sub.setCameraViewports({}));
        camera.identity=uint64_t(sub.activeViewportId()+1);
        camera.position={float(sub.activeViewportId()),0,-5};
        camera.spaceRegionId=uint32_t(sub.activeViewportId()+1);
        camera.view=ayt::math::lh::lookAt(camera.position,camera.position+ayt::math::FVector3{0,0,1},{0,1,0});
        camera.projection=ayt::math::lh::perspective(1.f,sub.viewportAspect(),.1f,100.f); return true;
    });
    sub.addSceneBuilderForOwner(&owner,[&](RenderScene&) {
        CHECK_FALSE(sub.setCameraViewports({}));
        observed.push_back(sub.activeViewportId()); aspects.push_back(sub.viewportAspect());
        CHECK_FLOAT_EQ(sub.renderer().mainCameraPosition().x,float(sub.activeViewportId()),0);
    });
    sub.update(0); CHECK(sub.cameraViewPacketCount()==2);
    CHECK(observed.size()==2); CHECK(observed[0]==0); CHECK(observed[1]==1);
    CHECK_FLOAT_EQ(aspects[0],2.f/3.f,1e-5f); CHECK_FLOAT_EQ(aspects[1],4.f/3.f,1e-5f);
    CHECK(sub.activeViewportId()==0); CHECK(sub.viewportWidth()==320);
    auto bad=recipes; bad[1].id=0; CHECK_FALSE(sub.setCameraViewports(bad)); CHECK(sub.cameraViewports().size()==2);
    sub.clearSceneBuildersForOwner(&owner); observed.clear(); sub.update(0); CHECK(observed.empty());
    CHECK(sub.setCameraViewports({})); CHECK(sub.cameraViewports().empty()); sub.shutdown();
}

TEST_CASE(main_presentation_owner_replaces_world_and_restores_host_state) {
    int window=0,owner=0,other=0;
    RendererSubSystem::setWindowProvider({});RendererSubSystem::setBootstrapBackend(Backend::Noop);
    RendererSubSystem::setBootstrapWindow(&window,320,240);RendererSubSystem::setBootstrapViewport(12,16,280,200);
    RendererSubSystem sub;CHECK(sub.initialize());auto& renderer=sub.renderer();
    renderer.setAmbientStrength(.37f);renderer.setDepthHazeEnabled(true);renderer.setSsaoEnabled(true);renderer.setColorGradingEnabled(true);renderer.setTaaEnabled(true);VolumetricSettings hostFog;hostFog.enabled=true;hostFog.density=.123f;renderer.setVolumetricSettings(hostFog);
    renderer.setMainCameraLookAtPerspective({2,3,-4},{0,0,0},{0,1,0},50,1.4f,.1f,100);
    const auto eye=renderer.mainCameraPosition();
    PerformanceLight hostLight;hostLight.id=777;hostLight.light.type=LightType::Spot;hostLight.light.range=20;hostLight.light.coneCosInner=.98f;hostLight.light.coneCosOuter=.95f;hostLight.light.intensity=3;
    CHECK(renderer.setPerformanceLights(std::span<const PerformanceLight>(&hostLight,1)));
    int worldCalls=0,previewCalls=0;sub.setSceneBuilder([&](RenderScene&){++worldCalls;});
    sub.setMainPresentationForOwner(&owner,[&](Renderer& r,RenderScene&){
        sub.clearMainPresentationForOwner(&owner);CHECK(sub.hasMainPresentation());sub.setMainPresentationForOwner(&other,[](Renderer&,RenderScene&){CHECK(false);});
        ++previewCalls;r.setDepthHazeEnabled(false);r.setSsaoEnabled(false);r.setColorGradingEnabled(false);r.setFxaaEnabled(true);r.configurePipeline(RenderPipelineDesc::makeDeferred());r.setAmbientStrength(.02f);
        auto fog=r.volumetricSettings();fog.density=.8f;r.setVolumetricSettings(fog);
        r.setMainCameraLookAtPerspective({30,40,-50},{0,0,0},{0,1,0},50,1,.1f,100);
        r.setPerformanceLights({});r.setDebugOverlayEnabled(true);r.setDebugOverlaySuppressed(true);
    });
    CHECK(sub.hasMainPresentation());sub.setViewportRect(30,40,120,100);sub.update(0);CHECK(worldCalls==0);
    sub.renderCompositeFrame(true,nullptr,[](bool,CompositeUiPhase){});
    CHECK(previewCalls==1);CHECK(worldCalls==0);CHECK(renderer.pipelineDesc().path==RenderPath::Deferred);
    CHECK_FLOAT_EQ(renderer.volumetricSettings().density,.8f,1e-6f);CHECK(renderer.performanceLightStats().sourceLights==0);
    sub.clearMainPresentationForOwner(&other);CHECK(sub.hasMainPresentation());
    sub.clearMainPresentationForOwner(&owner);CHECK_FALSE(sub.hasMainPresentation());
    CHECK(renderer.depthHazeEnabled());CHECK(renderer.ssaoEnabled());CHECK(renderer.colorGradingEnabled());CHECK(renderer.taaEnabled());CHECK_FALSE(renderer.fxaaEnabled());
    CHECK(renderer.pipelineDesc().path==RenderPath::Forward);CHECK_FLOAT_EQ(renderer.ambientStrength(),.37f,1e-6f);
    CHECK_FLOAT_EQ(renderer.volumetricSettings().density,.123f,1e-6f);CHECK(renderer.performanceLightStats().sourceLights==1);
    CHECK_FALSE(renderer.isDebugOverlayEnabled());CHECK_FALSE(renderer.isDebugOverlaySuppressed());
    CHECK(sub.viewportWidth()==280);CHECK(sub.viewportHeight()==200);
    // Camera override resumes the host's main camera contract; extraction is fresh.
    sub.update(0);CHECK(worldCalls==1);
    sub.setMainPresentationForOwner(&owner,[](Renderer& r,RenderScene&){r.setAmbientStrength(.01f);});
    sub.renderCompositeFrame(true,nullptr,[](bool,CompositeUiPhase){});
    sub.setMainPresentationForOwner(&other,[](Renderer& r,RenderScene&){CHECK_FLOAT_EQ(r.ambientStrength(),.37f,1e-6f);});
    sub.clearMainPresentationForOwner(&owner);CHECK(sub.hasMainPresentation());
    sub.renderCompositeFrame(true,nullptr,[](bool,CompositeUiPhase){});
    sub.shutdown();CHECK_FALSE(sub.hasMainPresentation());
}
TEST_CASE(transparent_readback_normalizes_orientation_and_alpha) {
    CameraReadbackFrame source;
    source.width=2; source.height=2; source.originBottomLeft=true;
    // Backend row zero is the bottom row.
    source.pixels={64,32,0,128, 9,8,7,0,
                   255,128,0,255, 0,0,0,0};
    TransparentVideoFrame frame;
    CHECK(convertTransparentCameraFrame(source,123456789,frame));
    CHECK(frame.width==2 && frame.height==2);
    CHECK(frame.presentationTimeNs==123456789);
    CHECK(frame.pixels[0]==255 && frame.pixels[1]==128);
    CHECK(frame.pixels[8]==128 && frame.pixels[9]==64);
    CHECK(frame.pixels[12]==0 && frame.pixels[13]==0 &&
          frame.pixels[14]==0 && frame.pixels[15]==0);
    const auto original=frame.pixels;
    source.pixels.pop_back();
    CHECK_FALSE(convertTransparentCameraFrame(source,2,frame));
    CHECK(frame.pixels==original && frame.presentationTimeNs==123456789);
}

TEST_CASE(transparent_frame_writes_png_without_overwriting_existing_output) {
    TransparentVideoFrame frame{2,1,100,{255,0,0,255,0,0,0,0}};
    const auto unique=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path=std::filesystem::temp_directory_path()/
        ("aliyat-alpha-frame-"+std::to_string(unique)+".png");
    CHECK(writeTransparentVideoFramePng(frame,path.string()));
    std::ifstream input(path,std::ios::binary);
    std::array<unsigned char,8> signature{};
    input.read(reinterpret_cast<char*>(signature.data()),signature.size());
    CHECK(input.good());
    const std::array<unsigned char,8> expected{137,80,78,71,13,10,26,10};
    CHECK(signature==expected);
    const auto decoded=detail::decodeImageFile(path.string());
    CHECK(decoded.width==2 && decoded.height==1);
    CHECK(decoded.rgba8==frame.pixels);
    CHECK_FALSE(writeTransparentVideoFramePng(frame,path.string()));
    input.close();
    std::error_code error;
    std::filesystem::remove(path,error);
    CHECK(!error);
}
TEST_SUITE_END
}
