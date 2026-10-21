#include <AYTest.h>
#include "detail/TextureStorageAccounting.h"
#include <algorithm>
#include <limits>

using namespace ayt::render::detail;
TEST_SUITE(TextureStorageAccountingTests)
TEST_CASE(logical_block_sizes_match_independent_rectangular_and_narrow_mip_references)
{
    struct Format {bgfx::TextureFormat::Enum value;uint32_t pixel,block;};
    const Format formats[]={{bgfx::TextureFormat::RGBA8,4,0},{bgfx::TextureFormat::R8,1,0},
        {bgfx::TextureFormat::RG16F,4,0},{bgfx::TextureFormat::RGBA32F,16,0},
        {bgfx::TextureFormat::BC1,0,8},{bgfx::TextureFormat::BC3,0,16},
        {bgfx::TextureFormat::BC4,0,8},{bgfx::TextureFormat::BC5,0,16},{bgfx::TextureFormat::BC7,0,16}};
    const uint32_t sizes[][2]={{4,4},{5,3},{1,7},{7,1},{8,12}};
    for(const auto& format:formats)for(const auto& size:sizes){
        uint32_t width=size[0],height=size[1],levels=0;uint64_t expected=0;
        do{expected+=format.block?uint64_t((width+3)/4)*((height+3)/4)*format.block
                :uint64_t(width)*height*format.pixel;
            ++levels;if(width==1&&height==1)break;
            width=std::max(1u,width/2);height=std::max(1u,height/2);
        }while(true);
        uint64_t bytes=71;CHECK(textureLogicalStorageBytes(size[0],size[1],levels,false,format.value,bytes));
        CHECK(bytes==expected);
    }
    uint64_t cube=0;CHECK(textureLogicalStorageBytes(4,4,3,true,bgfx::TextureFormat::RGBA8,cube));
    CHECK(cube==6u*(64u+16u+4u));
}
TEST_CASE(invalid_shapes_formats_levels_and_overflow_preserve_the_output)
{
    uint64_t bytes=71;
    CHECK_FALSE(textureLogicalStorageBytes(0,4,1,false,bgfx::TextureFormat::RGBA8,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,3,1,true,bgfx::TextureFormat::RGBA8,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,4,0,false,bgfx::TextureFormat::RGBA8,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,4,4,false,bgfx::TextureFormat::RGBA8,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,4,1,false,bgfx::TextureFormat::Unknown,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,4,1,false,bgfx::TextureFormat::UnknownDepth,bytes));
    CHECK_FALSE(textureLogicalStorageBytes(4,4,1,false,static_cast<bgfx::TextureFormat::Enum>(UINT32_MAX),bytes));
    CHECK_FALSE(textureLogicalStorageBytes(UINT32_MAX,UINT32_MAX,1,true,bgfx::TextureFormat::RGBA32F,bytes));
    CHECK(bytes==71);
}
TEST_SUITE_END
