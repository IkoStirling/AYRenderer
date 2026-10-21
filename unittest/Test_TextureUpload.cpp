#include "AYTest.h"
#include "detail/TextureUploadPlan.h"
#include "detail/TextureImageLoader.h"
#include "AYResource/assetsImpl/Texture.h"

#include <algorithm>
#include <array>
#include <limits>
#include <filesystem>
#include <fstream>
#include <chrono>

namespace {
using namespace ayt::resource;
using namespace ayt::render::detail;

Texture chain(uint32_t width, uint32_t height, TextureFormat format, uint32_t count) {
    Texture texture;
    texture._width = width; texture._height = height; texture._format = format;
    std::vector<uint8_t> data;
    std::vector<uint32_t> offsets, sizes;
    for (uint32_t level = 0; level < count; ++level) {
        offsets.push_back(static_cast<uint32_t>(data.size()));
        const auto bytes = Texture::computeMipSize(width, height, format);
        sizes.push_back(bytes);
        data.resize(data.size() + bytes, static_cast<uint8_t>(level + 1));
        width = std::max(1u, width / 2u); height = std::max(1u, height / 2u);
    }
    texture.setImageData(std::move(data));
    texture.setMipmapLayout(std::move(offsets), std::move(sizes));
    return texture;
}
} // namespace

TEST_SUITE(TextureUploadPlanTests)

TEST_CASE(full_odd_and_narrow_chains_preserve_every_authored_level) {
    for (auto dimensions : {std::array<uint32_t,2>{5,3}, {1,7}, {7,1}, {1,1}}) {
        const auto count = dimensions[0] == 1 && dimensions[1] == 1 ? 1u : 3u;
        const auto source = chain(dimensions[0], dimensions[1], TextureFormat::RGBA8, count);
        const auto plan = prepareTextureUpload(source, false);
        CHECK(plan.isValid()); CHECK(plan.width == dimensions[0]); CHECK(plan.height == dimensions[1]);
        CHECK(plan.mipCount == count); CHECK(plan.sourceMipCount == count);
        CHECK_FALSE(plan.partialChainFallback);
        CHECK(plan.bytes.size() == source.getImageDataSize());
        CHECK(std::equal(plan.bytes.begin(), plan.bytes.end(), source.getImageDataBytes()));
    }
}

TEST_CASE(single_level_and_valid_partial_chains_never_expose_uninitialized_mips) {
    for (uint32_t count : {1u,2u}) {
        const auto source = chain(4,4,TextureFormat::RGBA8,count);
        const auto plan = prepareTextureUpload(source, false);
        CHECK(plan.isValid()); CHECK(plan.mipCount == 1); CHECK(plan.sourceMipCount == count);
        CHECK(plan.bytes.size() == 64); CHECK(plan.partialChainFallback == (count == 2));
        CHECK((count == 1) == plan.diagnostic.empty());
    }
}

TEST_CASE(invalid_tail_extra_levels_zero_and_oversized_dimensions_are_rejected) {
    auto source = chain(4,4,TextureFormat::RGBA8,2);
    source.setMipmapLayout({0,64},{64,8}); // Invalid tail must not be hidden by base-only fallback.
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source = chain(4,4,TextureFormat::RGBA8,4);
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source = chain(4,4,TextureFormat::RGBA8,1);
    source._width = 0; CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source._width = 65536; CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source._width = 4; source.setMipmapLayout({},{});
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source = chain(4,4,TextureFormat::RGBA8,1);
    source._format = static_cast<TextureFormat>(255);
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
}

TEST_CASE(native_formats_preserve_encoded_bytes_and_exact_block_sizes) {
    for (auto format : {TextureFormat::RGB8, TextureFormat::RGBA8, TextureFormat::BC1,
                       TextureFormat::BC3, TextureFormat::BC4, TextureFormat::BC5, TextureFormat::BC7}) {
        const auto source = chain(4,4,format,3);
        const auto plan = prepareTextureUpload(source,false);
        CHECK(plan.isValid()); CHECK(plan.mipCount == 3);
        CHECK(plan.bytes.size() == source.getImageDataSize());
        CHECK(std::equal(plan.bytes.begin(),plan.bytes.end(),source.getImageDataBytes()));
        CHECK((plan.flags & BGFX_TEXTURE_SRGB) == 0);
    }
}

TEST_CASE(bc_color_expansion_preserves_channels_at_all_levels_and_odd_edges) {
    for (bool two : {false,true}) {
        auto source = chain(5,3,two ? TextureFormat::BC5 : TextureFormat::BC4,3);
        std::vector<uint8_t> bytes(source.getImageDataSize(),0);
        size_t offset = 0;
        for (uint32_t level = 0; level < 3; ++level) {
            for (uint32_t i = 0; i < source.getMipmapSize(level); i += two ? 16 : 8) {
                bytes[offset+i] = bytes[offset+i+1] = static_cast<uint8_t>(64 + level * 32);
                if (two) bytes[offset+i+8] = bytes[offset+i+9] = static_cast<uint8_t>(192 - level * 32);
            }
            offset += source.getMipmapSize(level);
        }
        source.setImageData(std::move(bytes));
        const auto plan = prepareTextureUpload(source,true);
        CHECK(plan.isValid()); CHECK(plan.format == bgfx::TextureFormat::RGBA8);
        CHECK(plan.mipCount == 3); CHECK(plan.bytes.size() == 72);
        CHECK((plan.flags & BGFX_TEXTURE_SRGB) != 0);
        offset = 0;
        uint32_t width = 5, height = 3;
        for (uint32_t level = 0; level < 3; ++level) {
            for (uint32_t i = 0; i < width * height; ++i) {
                CHECK(plan.bytes[offset++] == 64 + level * 32);
                CHECK(plan.bytes[offset++] == (two ? 192 - level * 32 : 0));
                CHECK(plan.bytes[offset++] == 0); CHECK(plan.bytes[offset++] == 255);
            }
            width = std::max(1u,width/2); height = std::max(1u,height/2);
        }
    }
}

TEST_CASE(non_block_aligned_compressed_chains_expand_without_changing_dimensions) {
    for (auto format : {TextureFormat::BC1,TextureFormat::BC3,TextureFormat::BC4,TextureFormat::BC5,TextureFormat::BC7}) {
        const auto source = chain(5,3,format,3);
        const auto plan = prepareTextureUpload(source,false);
        CHECK(plan.isValid()); CHECK(plan.width == 5 && plan.height == 3);
        CHECK(plan.mipCount == 3); CHECK(plan.bytes.size() == 72);
        CHECK(plan.format == bgfx::TextureFormat::RGBA8); CHECK_FALSE(plan.diagnostic.empty());
    }
}

TEST_CASE(non_block_aligned_bc_color_decoding_preserves_alpha_and_refuses_bad_lengths) {
    std::array<uint8_t,16> bc1{};
    bc1[1] = 0xf8; // First block is opaque red.
    bc1[10] = bc1[11] = 255; // Second block: color0 < color1, index3 is transparent.
    for (size_t i = 12; i < 16; ++i) bc1[i] = 255;
    const auto image = decodeBcColor(5,3,bc1.data(),16,BcColorFormat::BC1);
    CHECK(image.isValid()); CHECK(image.rgba8.size() == 60);
    for (size_t y = 0; y < 3; ++y) {
        for (size_t x = 0; x < 4; ++x) {
            const size_t p = (y*5+x)*4;
            CHECK(image.rgba8[p] == 255); CHECK(image.rgba8[p+3] == 255);
        }
        CHECK(image.rgba8[(y*5+4)*4+3] == 0);
    }
    CHECK_FALSE(decodeBcColor(5,3,bc1.data(),15,BcColorFormat::BC1).isValid());
    CHECK_FALSE(decodeBcColor(65535,65535,bc1.data(),16,BcColorFormat::BC1).isValid());
    std::array<uint8_t,32> bc3{};
    for (size_t block : {0u,16u}) { bc3[block] = 128;bc3[block+1] = 64;bc3[block+8] = bc3[block+9] = 255; }
    const auto alpha = decodeBcColor(5,3,bc3.data(),32,BcColorFormat::BC3);
    CHECK(alpha.isValid());
    for (size_t p = 0; p < alpha.rgba8.size(); p += 4) {
        CHECK(alpha.rgba8[p] == 255);CHECK(alpha.rgba8[p+3] == 128);
    }
    CHECK_FALSE(decodeBcColor(5,3,bc3.data(),31,BcColorFormat::BC3).isValid());
}

TEST_CASE(authored_min_mag_and_independent_axis_addressing_map_to_creation_flags) {
    auto source = chain(4,4,TextureFormat::RGBA8,3);
    TextureSampler sampler;
    sampler.minFilter = TextureFilter::Point; sampler.magFilter = TextureFilter::Point;
    sampler.wrapU = TextureWrap::Mirror; sampler.wrapV = TextureWrap::Clamp;
    source.setSampler(sampler);
    auto plan = prepareTextureUpload(source,false);
    CHECK(plan.isValid()); CHECK_FALSE(plan.samplerFallback);
    CHECK((plan.flags & BGFX_SAMPLER_U_MASK) == BGFX_SAMPLER_U_MIRROR);
    CHECK((plan.flags & BGFX_SAMPLER_V_MASK) == BGFX_SAMPLER_V_CLAMP);
    CHECK((plan.flags & BGFX_SAMPLER_W_MASK) == 0);
    CHECK((plan.flags & BGFX_SAMPLER_MIN_MASK) == BGFX_SAMPLER_MIN_POINT);
    CHECK((plan.flags & BGFX_SAMPLER_MAG_MASK) == BGFX_SAMPLER_MAG_POINT);
    CHECK((plan.flags & BGFX_SAMPLER_MIP_MASK) == BGFX_SAMPLER_MIP_POINT);
    sampler.minFilter = TextureFilter::Bilinear; source.setSampler(sampler);
    plan = prepareTextureUpload(source,false);
    CHECK((plan.flags & BGFX_SAMPLER_MIN_MASK) == 0);
    CHECK((plan.flags & BGFX_SAMPLER_MIP_MASK) == BGFX_SAMPLER_MIP_POINT);
    sampler.minFilter = TextureFilter::Trilinear; source.setSampler(sampler);
    plan = prepareTextureUpload(source,false);
    CHECK((plan.flags & BGFX_SAMPLER_MIP_MASK) == 0);
}

TEST_CASE(rgb_srgb_expansion_keeps_native_rgb_and_opaque_alpha_at_every_level) {
    const auto source = chain(5,3,TextureFormat::RGB8,3);
    const auto plan = prepareTextureUpload(source,true);
    CHECK(plan.isValid()); CHECK(plan.format == bgfx::TextureFormat::RGBA8);
    CHECK(plan.mipCount == 3); CHECK(plan.bytes.size() == 72);
    size_t offset = 0;
    for (uint32_t level = 0; level < 3; ++level)
        for (uint32_t pixel = 0; pixel < source.getMipmapSize(level)/3; ++pixel) {
            for (int c = 0; c < 3; ++c) CHECK(plan.bytes[offset++] == level+1);
            CHECK(plan.bytes[offset++] == 255);
        }
}

TEST_CASE(unsupported_sampler_fields_have_finite_documented_fallbacks) {
    auto source = chain(4,4,TextureFormat::RGBA8,3);
    TextureSampler sampler;
    sampler.minFilter = TextureFilter::Anisotropic; sampler.anisotropy = 4;
    sampler.wrapU = TextureWrap::Border; sampler.lodBias = 1;
    source.setSampler(sampler);
    auto plan = prepareTextureUpload(source,false);
    CHECK(plan.isValid()); CHECK(plan.samplerFallback); CHECK_FALSE(plan.diagnostic.empty());
    CHECK((plan.flags & BGFX_SAMPLER_MIN_MASK) == BGFX_SAMPLER_MIN_ANISOTROPIC);
    CHECK((plan.flags & BGFX_SAMPLER_U_MASK) == BGFX_SAMPLER_U_CLAMP);
    sampler.magFilter = TextureFilter::Point; source.setSampler(sampler);
    plan = prepareTextureUpload(source,false);
    CHECK((plan.flags & BGFX_SAMPLER_MAG_MASK) == 0);
    CHECK(plan.diagnostic.find("linear magnification") != std::string::npos);
    sampler.anisotropy = std::numeric_limits<float>::quiet_NaN();
    sampler.magFilter = static_cast<TextureFilter>(255);
    sampler.wrapV = static_cast<TextureWrap>(255); source.setSampler(sampler);
    plan = prepareTextureUpload(source,false);
    CHECK(plan.isValid()); CHECK(plan.samplerFallback);
    CHECK((plan.flags & BGFX_SAMPLER_MIN_MASK) == 0);
    CHECK((plan.flags & BGFX_SAMPLER_MAG_MASK) == 0);
    CHECK((plan.flags & BGFX_SAMPLER_V_MASK) == 0);
}

TEST_CASE(resource_srgb_request_is_preserved_without_mutating_cpu_settings) {
    auto source = chain(4,4,TextureFormat::RGBA8,3);
    auto sampler = source.getSampler(); sampler.sRGB = true; source.setSampler(sampler);
    const auto plan = prepareTextureUpload(source,false);
    CHECK((plan.flags & BGFX_TEXTURE_SRGB) != 0);
    CHECK(source.getSampler().sRGB); CHECK(source.getMipmapCount() == 3);
}

TEST_CASE(oversized_advertised_data_is_refused_before_read_or_allocation) {
    auto source = chain(1,1,TextureFormat::RGBA8,1);
    source._width = source._height = 32768;
    source.setMipmapLayout({0},{UINT32_MAX});
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
    source._width = 16384; source._height = 4097;
    source.setMipmapLayout({0},{16384u*4097u*4u});
    CHECK_FALSE(prepareTextureUpload(source,false).isValid());
}

TEST_CASE(raw_image_fallback_refuses_large_advertised_header_before_pixel_expansion) {
    const std::array<uint8_t,33> bytes{
        137,80,78,71,13,10,26,10, 0,0,0,13, 73,72,68,82,
        0,0,0x27,0x10, 0,0,0x27,0x10, 8,6,0,0,0, 0,0,0,0};
    const auto path=std::filesystem::temp_directory_path()/("ay-large-image-header-"
        +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".png");
    struct Remove{std::filesystem::path path;~Remove(){std::error_code ec;std::filesystem::remove(path,ec);}} remove{path};
    {std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());CHECK(bool(stream));}
    CHECK_FALSE(decodeImageFile(path.string()).isValid());
    auto truncated=bytes;truncated[18]=truncated[22]=0;truncated[19]=truncated[23]=4;
    {std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream.write(reinterpret_cast<const char*>(truncated.data()),truncated.size());CHECK(bool(stream));}
    CHECK_FALSE(decodeImageFile(path.string()).isValid()); // Recognized small header, missing pixel payload.
}

TEST_SUITE_END
