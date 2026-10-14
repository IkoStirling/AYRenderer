#include "detail/RenderViewNamespaces.h"
#include <AYRenderer.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYTest.h>
#include <array>
#include <limits>

using namespace ayt::render;
namespace {
TEST_SUITE(CameraViews)
TEST_CASE(ui_and_preview_ranges_never_overlap_fixed_scene_or_runtime_camera_views) {
    using namespace ayt::render::detail;
    CHECK(uiViewCapacity()==145);
    CHECK(nextUiViewRange(32,1)==50); CHECK(nextUiViewRange(62,4)==112);
    CHECK(nextUiViewRange(64,1)==112); CHECK(nextUiViewRange(242,2)==UINT16_MAX);
    for(uint16_t next=32; next<=242;) {
        next=nextUiViewRange(next,1); if(next==UINT16_MAX) break;
        CHECK(next<kCameraFirstView || next>kCameraLastView);
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
TEST_SUITE_END
}
