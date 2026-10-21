#include <AYTest.h>
#include <AYResource/assetsImpl/Mesh.h>
#include "detail/MeshSubmissionInfoBuilder.h"

#include <limits>

namespace {
using namespace ayt::render;
using namespace ayt::resource;

void prepareTwoSections(Mesh& mesh) {
    const float positions[]={0,0,0,2,0,0,0,2,0,2,2,0};
    const uint32_t indices[]={0,1,2,1,3,2};
    mesh._setForTestVertexLayout(1u<<unsigned(MeshAttribute::Position),4,12);
    mesh._setForTestVertexData(positions,sizeof(positions));
    mesh._setForTestIndices(indices,6);
    const IMesh::Submesh sections[]={{0,3,0,0},{3,3,1,99}};
    mesh._setForTestSubmeshes(sections,2);
    mesh._addForTestMaterialSlot("red.mat");mesh._addForTestMaterialSlot("../blue.mat");
    mesh._setForTestSkinPalettes({{0,2},{2,1}},{7,11,19});
}
void checkRejected(const IMesh& mesh) {
    MeshSubmissionInfo output;
    output.vertexCount=71;output.indexCount=93;output.materialSlots={"last-good.mat"};
    MeshSubmissionSection sentinel;sentinel.firstIndex=5;sentinel.indexCount=9;
    sentinel.bonePalette={17,23};output.sections.push_back(sentinel);
    std::string error;
    CHECK_FALSE(ayt::render::detail::buildMeshSubmissionInfo(mesh,output,&error));
    CHECK(!error.empty());CHECK(output.vertexCount==71);CHECK(output.indexCount==93);
    CHECK(output.materialSlots==std::vector<std::string>{"last-good.mat"});
    CHECK(output.sections.size()==1);
    if(output.sections.size()==1){
        CHECK(output.sections[0].firstIndex==5);CHECK(output.sections[0].indexCount==9);
        CHECK(output.sections[0].bonePalette==std::vector<uint32_t>({17,23}));
    }
}
}

TEST_SUITE(MeshSubmissionInfoTests)
TEST_CASE(section_ranges_slots_centers_and_palettes_are_owned_independently_of_source)
{
    Mesh mesh;prepareTwoSections(mesh);MeshSubmissionInfo result;std::string error="old";
    CHECK(ayt::render::detail::buildMeshSubmissionInfo(mesh,result,&error));CHECK(error.empty());
    CHECK(result.vertexCount==4);CHECK(result.indexCount==6);CHECK(result.sections.size()==2);
    CHECK(result.materialSlots==std::vector<std::string>({"red.mat","../blue.mat"}));
    if(result.sections.size()!=2)return;
    const auto& a=result.sections[0];const auto& b=result.sections[1];
    CHECK(a.firstIndex==0);CHECK(a.indexCount==3);CHECK(a.materialIndex==0);
    CHECK(b.firstIndex==3);CHECK(b.indexCount==3);CHECK(b.materialIndex==1);CHECK(b.vertexOffset==99);
    CHECK_FLOAT_EQ(a.localCenter.x,2.0f/3.0f,1e-6f);CHECK_FLOAT_EQ(a.localCenter.y,2.0f/3.0f,1e-6f);
    CHECK_FLOAT_EQ(b.localCenter.x,4.0f/3.0f,1e-6f);CHECK_FLOAT_EQ(b.localCenter.y,4.0f/3.0f,1e-6f);
    CHECK(a.bonePalette==std::vector<uint32_t>({7,11}));CHECK(b.bonePalette==std::vector<uint32_t>{19});
    mesh.unload();CHECK(result.indexCount==6);CHECK(result.materialSlots[1]=="../blue.mat");
    CHECK(result.sections[0].bonePalette[1]==11);
}
TEST_CASE(empty_sections_keep_ordinal_identity_and_use_finite_authored_center)
{
    Mesh mesh;prepareTwoSections(mesh);
    const IMesh::Submesh sections[]={{6,0,0,0},{0,6,1,0}};
    mesh._setForTestSubmeshes(sections,2);mesh._setForTestBounds({9,8,7},{1,1,1});
    MeshSubmissionInfo output;CHECK(ayt::render::detail::buildMeshSubmissionInfo(mesh,output));
    CHECK(output.sections.size()==2);if(output.sections.size()!=2)return;
    CHECK(output.sections[0].indexCount==0);CHECK(output.sections[1].materialIndex==1);
    CHECK_FLOAT_EQ(output.sections[0].localCenter.x,9,0);
    CHECK_FLOAT_EQ(output.sections[0].localCenter.y,8,0);
    CHECK_FLOAT_EQ(output.sections[1].localCenter.x,1,1e-6f);
}
TEST_CASE(bad_indices_and_wrapping_section_ranges_leave_output_unchanged)
{
    Mesh mesh;prepareTwoSections(mesh);
    const uint32_t indices[]={0,1,UINT32_MAX,1,3,2};mesh._setForTestIndices(indices,6);checkRejected(mesh);
    Mesh ranges;prepareTwoSections(ranges);
    const IMesh::Submesh sections[]={{UINT32_MAX,3,0,0},{0,3,1,0}};
    ranges._setForTestSubmeshes(sections,2);checkRejected(ranges);
}
TEST_CASE(nonfinite_positions_and_invalid_position_layout_are_rejected)
{
    Mesh mesh;prepareTwoSections(mesh);
    const float positions[]={0,0,0,2,0,0,0,std::numeric_limits<float>::infinity(),0,2,2,0};
    mesh._setForTestVertexData(positions,sizeof(positions));checkRejected(mesh);
    struct BadOffset final:Mesh {
        bool broken=false;
        AttributeInfo getAttributeInfo(MeshAttribute attribute) const override {
            return broken&&attribute==MeshAttribute::Position?AttributeInfo{255,3}:Mesh::getAttributeInfo(attribute);
        }
    } bad;bad.createCube();bad.broken=true;checkRejected(bad);
    struct BadCount final:Mesh {
        bool broken=false;
        AttributeInfo getAttributeInfo(MeshAttribute attribute) const override {
            return broken&&attribute==MeshAttribute::Position?AttributeInfo{0,2}:Mesh::getAttributeInfo(attribute);
        }
    } shortPosition;shortPosition.createCube();shortPosition.broken=true;checkRejected(shortPosition);
}
TEST_CASE(palette_count_and_wrapping_ranges_are_rejected_before_publication)
{
    Mesh mesh;prepareTwoSections(mesh);mesh._setForTestSkinPalettes({{0,2}},{7,11});checkRejected(mesh);
    Mesh range;prepareTwoSections(range);range._setForTestSkinPalettes({{UINT32_MAX,2},{0,1}},{7,11});checkRejected(range);
}
TEST_CASE(huge_declared_input_and_metadata_counts_are_rejected_before_data_walk)
{
    struct HugeInput final:Mesh {
        uint32_t getVertexCount() const override{return UINT32_MAX;}
        uint32_t getVertexStride() const override{return UINT32_MAX;}
        uint32_t getIndexCount() const override{return UINT32_MAX;}
    } huge;huge.createCube();checkRejected(huge);
    struct HugeSections final:Mesh {
        uint32_t getSubmeshCount() const override{return UINT32_MAX;}
    } sections;sections.createCube();checkRejected(sections);
    struct HugeSlots final:Mesh {
        uint32_t getMaterialSlotCount() const override{return UINT32_MAX;}
    } slots;slots.createCube();checkRejected(slots);
}
TEST_CASE(overlapping_sections_cannot_cause_unbounded_center_work)
{
    Mesh mesh;mesh.createCube();
    const std::vector<uint32_t> indices(262143,0);
    mesh._setForTestIndices(indices.data(),uint32_t(indices.size()));
    const std::vector<IMesh::Submesh> sections(257,IMesh::Submesh{0,uint32_t(indices.size()),0,0});
    mesh._setForTestSubmeshes(sections.data(),uint32_t(sections.size()));checkRejected(mesh);
}
TEST_CASE(oversized_material_path_does_not_partially_replace_output)
{
    Mesh mesh;mesh.createCube();mesh._addForTestMaterialSlot(std::string(65537,'x'));checkRejected(mesh);
}
TEST_SUITE_END
