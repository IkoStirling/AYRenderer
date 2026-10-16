#include <AYRenderer.h>
#include <AYRenderer/EmissivePointCloud.h>
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
TEST_SUITE(PerformanceLightingTests)
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
