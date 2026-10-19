#include <AYRenderer.h>
#include <AYRenderer/EmissivePointCloud.h>
#include <AYRenderer/FixtureEmitters.h>
#include <AYMath/MathUtils.h>
#include <AYRenderer/PerformanceShaderSources.h>
#include <AYShader/Phoskia.h>
#include <AYShader/IR.h>
#include <AYTest.h>
#include <limits>
#include <cmath>

using namespace ayt::render;
namespace {
PerformanceLight light(uint32_t id) {
    PerformanceLight p;p.id=id;p.light=Light::spot({0,2,0},{0,-1,0},10,20,.95f,.8f,{1,0,0});return p;
}
}
TEST_SUITE(FixtureEmitterTests)
TEST_CASE(front_side_back_and_physical_aperture_orientation_are_consistent) {
    auto p=light(7);p.light.position={0,0,0};p.light.spotDirection={0,0,-1};p.light.intensity=1;p.apertureRadius=.3f;
    const auto projection=ayt::math::lh::perspective(ayt::math::radians(55.f),1,.1f,1000);
    const auto view=[&](ayt::math::FVector3 eye){return ayt::math::lh::lookAt(eye,{0,0,0},{0,1,0});};
    FixtureEmitterSettings settings;settings.minimumPixelRadius=0;ParticleDrawData front,side,back,far;FixtureEmitterStats stats;
    CHECK(buildFixtureEmitterBatch({&p,1},view({0,0,-3}),projection,128,128,settings,{},front,stats));
    CHECK(stats.prepared==1);CHECK(front.vertices.size()==4);CHECK(front.indices.size()==6);CHECK(front.world3D);
    CHECK(front.vertices[0].r>1);for(const auto& vertex:front.vertices)CHECK(std::abs(vertex.z)<1e-6f);
    CHECK(std::abs(front.vertices[0].r-1/(3.141592653589793*.09))<1e-5);
    CHECK(buildFixtureEmitterBatch({&p,1},view({3,0,-3}),projection,128,128,settings,{},side,stats));
    CHECK(stats.prepared==1);CHECK(side.vertices[0].r<front.vertices[0].r*.01f);
    CHECK(buildFixtureEmitterBatch({&p,1},view({0,0,3}),projection,128,128,settings,{},back,stats));
    CHECK(stats.backFacing==1);CHECK(back.vertices.empty());
    CHECK(buildFixtureEmitterBatch({&p,1},view({0,0,-6}),projection,128,128,settings,{},far,stats));
    CHECK(far.vertices[0].r==front.vertices[0].r); // Resolved radiance has no extra distance-squared dimming.
    settings.offAxisGlow=0;
    CHECK(buildFixtureEmitterBatch({&p,1},view({3,0,-3}),projection,128,128,settings,{},side,stats));CHECK(side.vertices.empty());
}
TEST_CASE(iris_zoom_flux_and_subpixel_footprint_keep_energy_rules) {
    auto p=light(1);p.light.position={0,0,0};p.light.spotDirection={0,0,-1};p.light.intensity=1;p.apertureRadius=.03f;
    const auto view=ayt::math::lh::lookAt({0,0,-30},{0,0,0},{0,1,0});
    const auto projection=ayt::math::lh::perspective(ayt::math::radians(55.f),1,.1f,1000);
    FixtureEmitterSettings settings;settings.minimumPixelRadius=0;ParticleDrawData real,filtered,iris,flux;FixtureEmitterStats stats;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},real,stats));
    settings.minimumPixelRadius=2;CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},filtered,stats));
    const auto energy=[](const ParticleDrawData& b){const float width=b.vertices[1].x-b.vertices[0].x;return b.vertices[0].r*width*width;};
    CHECK(std::abs(energy(real)-energy(filtered))<1e-5f);CHECK(filtered.vertices[0].r<real.vertices[0].r);
    settings.minimumPixelRadius=0;p.iris=.5f;CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},iris,stats));
    CHECK(std::abs(energy(iris)/energy(real)-.25f)<1e-5f);
    p.iris=1;p.constantFlux=true;CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},flux,stats));
    CHECK(std::abs(flux.vertices[0].r/real.vertices[0].r-performanceLightFluxScale(p))<1e-6f);
}
TEST_CASE(global_local_and_animated_medium_extinguish_visible_emitter) {
    auto p=light(1);p.light.position={0,0,0};p.light.spotDirection={0,0,-1};p.light.intensity=1;p.apertureRadius=.3f;
    const auto view=ayt::math::lh::lookAt({0,0,-3},{0,0,0},{0,1,0});
    const auto projection=ayt::math::lh::perspective(ayt::math::radians(55.f),1,.1f,1000);
    ParticleDrawData clear,fogged,local,animated,repeat;FixtureEmitterStats stats;VolumetricSettings fog;fog.enabled=true;fog.density=.2f;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{}, {},clear,stats));
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{},fog,fogged,stats));
    CHECK(std::abs(fogged.vertices[0].r/clear.vertices[0].r-std::exp(-.6f))<1e-5f);
    fog.local=true;fog.center={0,0,-1.5f};fog.halfExtent={10,10,.5f};
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{},fog,local,stats));
    CHECK(std::abs(local.vertices[0].r/clear.vertices[0].r-std::exp(-.175f))<.002f);
    fog.noiseStrength=.8f;fog.noiseTime=5;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{},fog,animated,stats));
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{},fog,repeat,stats));
    CHECK(animated.vertices[0].r==repeat.vertices[0].r);CHECK(animated.vertices[0].r<clear.vertices[0].r);
    fog.center={50,0,0};CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{},fog,repeat,stats));
    CHECK(repeat.vertices[0].r==clear.vertices[0].r);
}
TEST_CASE(validation_is_transactional_and_hdr_is_bounded_at_tiny_apertures) {
    auto p=light(1);p.light.position={0,0,0};p.light.spotDirection={0,0,-1};p.apertureRadius=.1f;
    const auto view=ayt::math::lh::lookAt({0,0,-3},{0,0,0},{0,1,0});
    const auto projection=ayt::math::lh::perspective(ayt::math::radians(55.f),1,.1f,1000);
    ParticleDrawData batch;FixtureEmitterStats stats;CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{}, {},batch,stats));
    const auto old=batch.vertices[0].r;FixtureEmitterSettings settings;settings.maximumRadiance=std::numeric_limits<float>::infinity();
    CHECK(!buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},batch,stats));CHECK(batch.vertices[0].r==old);CHECK(stats.prepared==1);
    CHECK(!buildFixtureEmitterBatch({&p,1},view,projection,0,128,{}, {},batch,stats));
    auto invalid=view;invalid(0,0)=std::numeric_limits<float>::quiet_NaN();
    CHECK(!buildFixtureEmitterBatch({&p,1},invalid,projection,128,128,{}, {},batch,stats));CHECK(batch.vertices[0].r==old);
    settings={};settings.minimumPixelRadius=0;settings.maximumRadiance=100;p.apertureRadius=1e-8f;p.light.intensity=100000;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},batch,stats));CHECK(stats.clamped==1);CHECK(batch.vertices[0].r==100);
    p.apertureRadius=1e-24f;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,{},batch,stats));CHECK(std::isfinite(batch.vertices[0].r));
    VolumetricSettings fog;fog.enabled=true;fog.density=.2f;
    CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,settings,fog,batch,stats));
    CHECK(std::abs(batch.vertices[0].r-100*std::exp(-.6f))<1e-4f);
    p.apertureRadius=0;CHECK(buildFixtureEmitterBatch({&p,1},view,projection,128,128,{}, {},batch,stats));CHECK(batch.vertices.empty());
    p.apertureRadius=.1f;std::vector<PerformanceLight> lights(256,p);for(uint32_t i=0;i<256;++i)lights[i].id=i;
    CHECK(buildFixtureEmitterBatch(lights,view,projection,128,128,{}, {},batch,stats));CHECK(stats.prepared==256);CHECK(batch.vertices.size()==1024);
    lights.push_back(p);CHECK(!buildFixtureEmitterBatch(lights,view,projection,128,128,{}, {},batch,stats));CHECK(stats.prepared==256);
}
TEST_SUITE_END

TEST_SUITE(PerformanceLightingTests)
TEST_CASE(constant_flux_zoom_preserves_angular_integral_and_defaults_off) {
    auto p=light(1);CHECK(performanceLightFluxScale(p)==1);p.constantFlux=true;
    const double reference=2.0-std::cos(2.0*3.141592653589793/180)-std::cos(3.0*3.141592653589793/180);
    for(float outer:{.02f,.052359878f,.3f,1.2f}){
        p.light.coneCosInner=std::cos(outer*.6666667f);p.light.coneCosOuter=std::cos(outer);
        const double integral=2.0-double(p.light.coneCosInner)-p.light.coneCosOuter;
        CHECK(std::abs(performanceLightFluxScale(p)*integral-reference)<1e-9);
    }
    p.light=Light::point({0,0,0},5,1,{1,1,1});CHECK(performanceLightFluxScale(p)==1);
}
TEST_CASE(point_aggregation_preserves_rgb_energy_centroid_and_input_order) {
    std::vector<EmissivePoint> points{{9,{1,1,1},{1,0,0},4,.1f},{2,{3,1,1},{0,0,1},8,.1f},
        {7,{-1,1,1},{0,1,0},2,.1f},{8,{40,0,0},{1,1,1},0,.1f}};
    EmissivePointLightingSettings settings;settings.intensityScale=1;
    std::vector<PerformanceLight> lights,reverse;EmissivePointLightingStats stats;
    CHECK(buildEmissivePointLights(points,settings,lights,stats));
    CHECK(stats.source==4);CHECK(stats.contributing==3);CHECK(stats.occupiedCells==2);CHECK(lights.size()==2);
    const auto& mix=lights[1].light;CHECK(mix.type==LightType::Point);
    CHECK(std::abs(mix.color.x*mix.intensity-4)<1e-5f);CHECK(std::abs(mix.color.z*mix.intensity-8)<1e-5f);
    const double x=(1*.2126*4+3*.0722*8)/(.2126*4+.0722*8);
    CHECK(std::abs(mix.position.x-x)<1e-5);CHECK(mix.range>settings.range);
    std::reverse(points.begin(),points.end());CHECK(buildEmissivePointLights(points,settings,reverse,stats));
    CHECK(reverse[1].light.position.x==mix.position.x);CHECK(reverse[1].light.color.z==mix.color.z);
    PerformanceLighting packet;CHECK(packet.assign(lights,{}));CHECK(packet.stats.shadowLights==0);
    ayt::math::FVector3 center,extent;performanceLightBounds(packet.lights[1],center,extent);
    CHECK(center.x==mix.position.x);CHECK(extent.x==mix.range);CHECK(extent.z==mix.range);
}
TEST_CASE(point_aggregation_selects_brightest_cells_and_rejects_bad_values_transactionally) {
    std::vector<EmissivePoint> points{{1,{1,0,0},{1,0,0},1,.1f},{2,{21,0,0},{1,0,0},10,.1f}};
    EmissivePointLightingSettings settings;settings.maxLights=1;settings.intensityScale=1;
    std::vector<PerformanceLight> lights;EmissivePointLightingStats stats;
    CHECK(buildEmissivePointLights(points,settings,lights,stats));
    CHECK(lights.size()==1);CHECK(lights[0].light.position.x==21);CHECK(stats.budgetSkipped==1);
    points[0].id=2;CHECK(!buildEmissivePointLights(points,settings,lights,stats));CHECK(lights[0].light.position.x==21);
    points[0].id=1;points[0].intensity=std::numeric_limits<float>::infinity();
    CHECK(!buildEmissivePointLights(points,settings,lights,stats));CHECK(stats.budgetSkipped==1);
    points[0].intensity=1;settings.cellSize=0;CHECK(!buildEmissivePointLights(points,settings,lights,stats));
    settings.cellSize=8;settings.maxLights=65;CHECK(!buildEmissivePointLights(points,settings,lights,stats));
    settings.maxLights=0;CHECK(buildEmissivePointLights(points,settings,lights,stats));CHECK(lights.empty());
    settings.maxLights=16;points[0].color={10000,10000,10000};points[0].intensity=100000;settings.intensityScale=100;
    CHECK(buildEmissivePointLights(points,settings,lights,stats));CHECK(stats.clampedCells==1);
    PerformanceLighting packet;CHECK(packet.assign(lights,{}));
    auto point=lights[0];point.light.castShadow=true;CHECK(!packet.assign({&point,1},{}));
    point.light.castShadow=false;point.volumetric=1;CHECK(!packet.assign({&point,1},{}));
}

TEST_CASE(point_aggregation_caps_sparse_maximum_input_and_ties_by_cell_coordinate) {
    std::vector<EmissivePoint> points(65536);
    for(uint32_t i=0;i<points.size();++i){points[i].id=i;points[i].position={float(i),0,0};points[i].intensity=1;}
    EmissivePointLightingSettings settings;settings.cellSize=1;settings.maxLights=64;
    std::vector<PerformanceLight> lights,shuffled;EmissivePointLightingStats stats;
    CHECK(buildEmissivePointLights(points,settings,lights,stats));CHECK(lights.size()==64);
    CHECK(stats.occupiedCells==65536);CHECK(stats.budgetSkipped==65472);
    CHECK(lights.front().light.position.x==0);CHECK(lights.back().light.position.x==63);
    std::reverse(points.begin(),points.end());CHECK(buildEmissivePointLights(points,settings,shuffled,stats));
    CHECK(shuffled.front().light.position.x==0);CHECK(shuffled.back().light.position.x==63);
    points.push_back(points[0]);CHECK(!buildEmissivePointLights(points,settings,shuffled,stats));CHECK(shuffled.size()==64);
}
TEST_CASE(finite_aperture_expands_conservative_bounds_and_is_validated) {
    auto p=light(1);p.light.position={0,0,0};p.light.spotDirection={0,0,1};
    p.apertureRadius=.2f;ayt::math::FVector3 center,extent;performanceLightBounds(p,center,extent);
    CHECK(std::abs(center.z-5)<1e-5f);CHECK(std::abs(extent.z-5)<1e-5f);
    CHECK(std::abs(extent.x-(.2f+10*std::tan(std::acos(.8f))))<1e-4f);
    PerformanceLighting packet;CHECK(packet.assign({&p,1},{}));CHECK(packet.lights[0].apertureRadius==.2f);
    p.apertureRadius=-.1f;CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].apertureRadius==.2f);
    p.apertureRadius=std::numeric_limits<float>::quiet_NaN();CHECK(!packet.assign({&p,1},{}));
}
TEST_CASE(pattern_focus_and_iris_are_bounded_and_preserve_packet_on_failure) {
    auto p=light(1);PerformanceLighting packet;
    p.patternFocus=.4f;p.iris=.7f;
    CHECK(packet.assign({&p,1},{}));CHECK(packet.lights[0].patternFocus==.4f);
    p.iris=0;CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].iris==.7f);
    p.iris=1;p.patternFocus=std::numeric_limits<float>::quiet_NaN();
    CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].patternFocus==.4f);
}
TEST_CASE(texture_gobo_requires_a_handle_and_preserves_previous_packet) {
    PerformanceLighting packet;auto p=light(1);
    CHECK(packet.assign({&p,1},{}));
    p.pattern=LightPattern::Texture;
    CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].pattern==LightPattern::Open);
    p.goboTexture={42};
    CHECK(packet.assign({&p,1},{}));CHECK(packet.lights[0].goboTexture.id==42);
    Renderer renderer;std::string error;
    auto open=light(9);CHECK(renderer.setPerformanceLights({&open,1},{},&error));
    CHECK(!renderer.setPerformanceLights({&p,1},{},&error));
    CHECK(!error.empty());CHECK(renderer.performanceLightStats().sourceLights==1);
}
TEST_CASE(shadow_softness_is_bounded_and_transactional) {
    PerformanceLighting packet;auto p=light(1);p.light.castShadow=true;
    p.shadowSoftness=2;CHECK(packet.assign({&p,1},{}));CHECK(packet.lights[0].shadowSoftness==2);
    p.shadowSoftness=5;CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].shadowSoftness==2);
    p.shadowSoftness=std::numeric_limits<float>::quiet_NaN();CHECK(!packet.assign({&p,1},{}));
}
TEST_CASE(budgets_are_independent_and_priority_is_stable_with_shadow_mapping) {
    std::vector<PerformanceLight> lights;for(uint32_t i=0;i<32;++i){auto p=light(i);p.light.castShadow=true;lights.push_back(p);}
    lights[8].priority=2;lights[10].priority=2;PerformanceLighting packet;
    CHECK(packet.assign(lights,{32,16,4}));CHECK(packet.lights.size()==32);CHECK(packet.lights[0].id==8);CHECK(packet.lights[1].id==10);
    CHECK(packet.shadowSources.count==4);CHECK(packet.stats.shadowLights==4);
    CHECK(packet.shadowSlots[0]==0);CHECK(packet.shadowSlots[3]==3);CHECK(packet.shadowSlots[4]==-1);
    CHECK(packet.shadowSources.lights[0].color.x==0);CHECK(packet.budget.volumeLights==16);
}
TEST_CASE(invalid_packets_preserve_the_previous_snapshot) {
    PerformanceLighting packet;auto p=light(1);CHECK(packet.assign({&p,1},{}));
    p.light.spotDirection={0,0,0};CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights.size()==1);CHECK(packet.lights[0].light.spotDirection.y==-1);
    p=light(1);p.volumetric=std::numeric_limits<float>::quiet_NaN();CHECK(!packet.assign({&p,1},{}));
    p=light(1);CHECK(!packet.assign({&p,1},{257,16,4}));CHECK(!packet.assign({&p,1},{64,65,4}));CHECK(!packet.assign({&p,1},{64,16,9}));
    auto lights=std::vector{p,p};CHECK(!packet.assign(lights,{}));CHECK(packet.assign({},{}));CHECK(packet.lights.empty());
}
TEST_CASE(projected_range_scissors_cull_offscreen_lights_and_preserve_intersections) {
    auto p=light(1);p.light.range=.1f;p.light.position={0,0,0};auto vp=ayt::math::Float4x4::identity();
    auto rect=performanceLightRect(p,vp,100,100);CHECK(rect.visible);CHECK(rect.width<100);CHECK(rect.height<100);
    p.light.position.x=100;CHECK(!performanceLightRect(p,vp,100,100).visible);
    p.light.position={0,0,0};p.light.range=10;rect=performanceLightRect(p,vp,100,100);CHECK(rect.visible);CHECK(rect.width==100);
    CHECK(!performanceLightRect(p,vp,0,100).visible);
}
TEST_CASE(point_batches_keep_hdr_color_and_split_uint16_indices_without_per_point_materials) {
    std::vector<EmissivePoint> points(20000);for(uint32_t i=0;i<points.size();++i){points[i].id=i;points[i].position={0,0,.5f};}
    points[0].color={2,0,0};points[0].intensity=8;
    std::vector<ParticleDrawData> batches;EmissivePointStats stats;
    const auto identity=ayt::math::Float4x4::identity();
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,{},batches,stats));
    CHECK(stats.visible==20000);CHECK(stats.batches==2);CHECK(batches[0].vertices.size()==16383*4);
    CHECK(batches[0].vertices[0].r==16);CHECK(batches[0].indices.back()<65532);
    CHECK(batches[1].vertices.size()==(20000-16383)*4);
}
TEST_CASE(point_quality_budget_selects_stable_ids_independent_of_input_order) {
    std::vector<EmissivePoint> points(100);
    for(uint32_t i=0;i<points.size();++i){points[i].id=i;points[i].position={float(i)/100.f,0,.5f};points[i].radius=.01f;}
    const auto identity=ayt::math::Float4x4::identity();
    EmissivePointSettings settings;settings.maxPoints=10;
    std::vector<ParticleDrawData> first,second;EmissivePointStats stats;
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,settings,first,stats));
    CHECK(stats.visible==10);CHECK(stats.budgetSkipped==90);
    std::vector<float> firstPositions;
    for(size_t i=0;i<10;++i)firstPositions.push_back(first[0].vertices[i*4].x);
    std::reverse(points.begin(),points.end());
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,settings,second,stats));
    std::vector<float> secondPositions;
    for(size_t i=0;i<10;++i)secondPositions.push_back(second[0].vertices[i*4].x);
    std::sort(firstPositions.begin(),firstPositions.end());std::sort(secondPositions.begin(),secondPositions.end());
    CHECK(firstPositions==secondPositions);
}
TEST_CASE(point_culling_budgets_and_subpixel_coverage_are_explicit) {
    std::vector<EmissivePoint> points(3);points[1].position.x=100;points[2].intensity=0;
    std::vector<ParticleDrawData> batches;EmissivePointStats stats;const auto identity=ayt::math::Float4x4::identity();
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,{},batches,stats));CHECK(stats.visible==1);CHECK(stats.culled==2);
    points[0].radius=.0001f;
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,{},batches,stats));
    CHECK(batches[0].vertices[1].x-batches[0].vertices[0].x>.029f);CHECK(batches[0].vertices[0].r<4);
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,{.75f,true,0},batches,stats));CHECK(stats.visible==0);CHECK(stats.budgetSkipped==1);
    points[0].radius=-1;stats.visible=9;CHECK(!buildEmissivePointBatches(points,identity,identity,100,100,{},batches,stats));CHECK(stats.visible==9);
}
TEST_CASE(point_instances_preserve_cpu_selection_hdr_and_bounded_batches) {
    std::vector<EmissivePoint> points(20000);
    for(uint32_t i=0;i<points.size();++i){points[i].id=i;points[i].position={0,0,.5f};}
    points[0].color={2,0,0};points[0].intensity=8;
    const auto identity=ayt::math::Float4x4::identity();
    std::vector<ParticleDrawData> cpu,instances;EmissivePointStats cpuStats,instanceStats;
    CHECK(buildEmissivePointBatches(points,identity,identity,100,100,{},cpu,cpuStats));
    CHECK(buildEmissivePointInstanceBatches(points,identity,identity,100,100,{},instances,instanceStats));
    CHECK(instanceStats.visible==cpuStats.visible);CHECK(instanceStats.batches==2);
    CHECK(instanceStats.instanced);CHECK(instances[0].instanceStride==64);
    CHECK(instances[0].instanceData.size()==16383*16);
    CHECK(instances[1].instanceData.size()==(20000-16383)*16);
    CHECK(instances[0].vertices.empty());CHECK(instances[0].indices.empty());
    CHECK(instances[0].instanceData[0]==points[0].position.x);
    CHECK(instances[0].instanceData[3]==points[0].radius);
    CHECK(instances[0].instanceData[12]==cpu[0].vertices[0].r);
    EmissivePointSettings budget;budget.maxPoints=17;
    CHECK(buildEmissivePointInstanceBatches(points,identity,identity,100,100,budget,instances,instanceStats));
    CHECK(instanceStats.visible==17);CHECK(instanceStats.budgetSkipped==20000-17);
    CHECK(instances[0].instanceData.size()==17*16);
    points[0].radius=-1;instanceStats.visible=9;
    CHECK(!buildEmissivePointInstanceBatches(points,identity,identity,100,100,{},instances,instanceStats));
    CHECK(instanceStats.visible==9);
}
TEST_CASE(instanced_point_shader_uses_production_frontend) {
    ayt::shader::phoskia::Compiler compiler;ayt::shader::phoskia::ir::IRProgram program;
    std::vector<std::string> errors;
    const bool success=compiler.generateIr(kEmissivePointInstancedPhoskiaSource,{},program,errors);
    for(const auto& error:errors)std::fprintf(stderr,"%s\n",error.c_str());
    CHECK(success);
}
TEST_CASE(surface_shader_uses_production_frontend_and_bounded_procedural_patterns) {
    ayt::shader::phoskia::Compiler compiler;ayt::shader::phoskia::ir::IRProgram program;std::vector<std::string> errors;
    const bool success=compiler.generateIr(kPerformanceSurfaceSource,{},program,errors);
    for(const auto& e:errors)std::fprintf(stderr,"%s\n",e.c_str());CHECK(success);
}
TEST_SUITE_END

TEST_SUITE(PhotometryTests)
TEST_CASE(lm63_type_c_sampling_symmetry_and_transactional_rejection) {
    const std::string data="IESNA:LM-63-2002\n[TEST] measured\nTILT=NONE\n1 1000 2 3 2 1 2 .1 .2 .3\n1 1 20\n0 45 90\n0 180\n10 20 0\n30 40 0\n";
    IesProfile profile;std::string error;CHECK(IesProfile::parse(data,profile,&error));CHECK(error.empty());
    CHECK(profile.peakCandela()==80);CHECK(profile.sample(0,0)==20);CHECK(profile.sample(0,90)==40);
    CHECK(profile.sample(0,270)==40);CHECK(profile.sample(0,-90)==40);CHECK(profile.sample(45,180)==80);
    CHECK(profile.sample(91,0)==0);CHECK(profile.sample(-1,0)==0);
    auto bad=data;bad.replace(bad.find("TILT=NONE"),9,"TILT=INCLUDE");CHECK(!IesProfile::parse(bad,profile,&error));CHECK(profile.peakCandela()==80);
    CHECK(!IesProfile::parse(data+"nan",profile));CHECK(!IesProfile::parse(data+"42",profile));
    bad=data;bad.replace(bad.find("0 45 90"),7,"0 0 90");CHECK(!IesProfile::parse(bad,profile));
    bad=data;bad.replace(bad.find("3 2 1 2"),7,"3 2 2 2");CHECK(!IesProfile::parse(bad,profile));
    std::vector<uint8_t> atlas;CHECK(profile.angularTexture(atlas));CHECK(atlas.size()==257*129*4);
    for(int y=0;y<129;++y)CHECK(atlas[y*257*4]==atlas[(y*257+256)*4]);
    CHECK(profile.projection(.2f,32,atlas));CHECK(atlas.size()==32*32*4);CHECK(!profile.projection(0,32,atlas));
}
TEST_CASE(physical_flux_intensity_distance_color_and_legacy_compatibility) {
    auto p=light(1);p.light.color={1,1,1};p.light.intensity=100;
    CHECK(performanceLightIntensity(p)==100);p.constantFlux=true;CHECK(std::abs(performanceLightIntensity(p)-100*performanceLightFluxScale(p))<1e-6);
    p.intensityUnit=LightIntensityUnit::Candela;CHECK(performanceLightIntensity(p)==100);
    CHECK(illuminanceLux(100,2)==25);CHECK(illuminanceLux(100,4)==6.25f);CHECK(illuminanceLux(100,2,.5f)==12.5f);CHECK(illuminanceLux(100,0)==0);
    p.intensityUnit=LightIntensityUnit::Lumen;const auto solid=spotSolidAngle(.95f,.8f);
    CHECK(std::abs(performanceLightIntensity(p)*solid-100)<1e-4f);
    p.light.type=LightType::Point;CHECK(std::abs(performanceLightIntensity(p)*12.5663706f-100)<1e-4f);
    p.light.color={1,0,0};CHECK(std::abs(performanceLightIntensity(p)*12.5663706f*.2126f-100)<1e-4f);
    p.light.color={0,0,0};CHECK(performanceLightIntensity(p)==0);
    CHECK(spotSolidAngle(.8f,.95f)==0);
    IesProfile profile;CHECK(IesProfile::parse("IESNA:LM-63-1995\nTILT=NONE\n1 -1 1 3 1 1 2 0 0 0\n1 1 10\n0 90 180\n0\n100 100 100\n",profile));
    CHECK(std::abs(profile.coneSolidAngle(.95f,.8f)-solid)<1e-4f);
    p=light(1);p.light.color={1,1,1};p.intensityUnit=LightIntensityUnit::Lumen;p.iesProfile=std::make_shared<IesProfile>(profile);p.iesTexture={42};
    CHECK(std::abs(performanceLightIntensity(p)*profile.coneSolidAngle(.95f,.8f)-20)<1e-4f);
    PerformanceLighting packet;CHECK(packet.assign({&p,1},{}));p.iesTexture={};CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights.size()==1);
}
TEST_SUITE_END

TEST_SUITE(FixtureOpticsTests)
TEST_CASE(finite_pupil_focus_uses_distance_and_iris_with_legacy_disabled) {
    auto p=light(1);p.apertureRadius=.1f;p.light.coneCosOuter=std::cos(.1f);p.focusDistance=10;
    CHECK(performanceLightFocusBlur(p,10)==0);CHECK(performanceLightFocusBlur(p,5)>0);CHECK(performanceLightFocusBlur(p,20)>0);
    const auto blur=performanceLightFocusBlur(p,5);CHECK(std::abs(blur-.05f/(.1f+5*std::tan(.1f)))<1e-6f);
    p.iris=.5f;CHECK(std::abs(performanceLightFocusBlur(p,5)-blur*.5f)<1e-6f);
    p.focusDistance=0;CHECK(performanceLightFocusBlur(p,5)==0);
    p.focusDistance=10;p.apertureRadius=0;CHECK(performanceLightFocusBlur(p,5)==0);
}
TEST_CASE(prism_rotates_real_lobes_preserves_total_angular_energy_and_bounds) {
    auto p=light(1);p.light.coneCosInner=std::cos(.02f);p.light.coneCosOuter=std::cos(.04f);p.apertureRadius=.03f;
    const auto central=p.light.spotDirection;CHECK(performanceLightAngular(p,central)==1);
    p.prismCount=4;p.prismAngle=.2f;CHECK(performanceLightAngular(p,central)==0);
    const auto lobe=performanceLightLobeDirection(p,0);CHECK(std::abs(lobe.length()-1)<1e-6f);CHECK(std::abs(performanceLightAngular(p,lobe)-.25f)<1e-5f);
    p.prismRotation=.785398163f;CHECK(performanceLightAngular(p,lobe)==0);const auto rotated=performanceLightLobeDirection(p,0);CHECK(std::abs(performanceLightAngular(p,rotated)-.25f)<1e-5f);
    ayt::math::FVector3 center,extent;performanceLightBounds(p,center,extent);
    for(uint8_t i=0;i<p.prismCount;++i){const auto end=p.light.position+performanceLightLobeDirection(p,i)*p.light.range;for(int a=0;a<3;++a)CHECK(std::abs(end[a]-center[a])<=extent[a]+1e-4f);}
    // Integrate a wider cone to avoid under-resolving a narrow beam in this
    // independent equal-solid-angle spherical quadrature.
    p.light.coneCosInner=.95f;p.light.coneCosOuter=.8f;double sum=0;constexpr int n=128,m=256;
    for(int y=0;y<n;++y){const float c=-1+(y+.5f)*2/n,r=std::sqrt(1-c*c);for(int x=0;x<m;++x){const float a=(x+.5f)*6.283185307f/m;sum+=performanceLightAngular(p,{r*std::cos(a),c,r*std::sin(a)});}}
    const double integrated=sum*12.566370614359172/(n*m);CHECK(std::abs(integrated-spotSolidAngle(.95f,.8f))<.002);
    PerformanceLighting packet;CHECK(packet.assign({&p,1},{}));p.prismCount=9;CHECK(!packet.assign({&p,1},{}));CHECK(packet.lights[0].prismCount==4);
    p.prismCount=4;p.focusDistance=-1;CHECK(!packet.assign({&p,1},{}));
}
TEST_SUITE_END
