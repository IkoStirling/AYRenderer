#include "AYTest.h"

#include "detail/IblPrecompute.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using ayt::render::detail::IblCpuSet;
using ayt::render::detail::buildIblCpuSet;

TEST_SUITE(AYRenderer_IblV2)

TEST_CASE(constant_cube_builds_stable_irradiance_prefilter_and_brdf_lut)
{
        constexpr uint16_t sourceSize = 4;
        std::vector<uint8_t> source(
            static_cast<size_t>(sourceSize) * sourceSize * 4u * 6u);
        for (size_t pixel = 0; pixel < source.size() / 4u; ++pixel) {
            source[pixel * 4u + 0u] = 64u;
            source[pixel * 4u + 1u] = 128u;
            source[pixel * 4u + 2u] = 192u;
            source[pixel * 4u + 3u] = 255u;
        }

        const IblCpuSet ibl = buildIblCpuSet(
            source.data(), sourceSize,
            /*irradianceSize=*/2u,
            /*prefilteredSize=*/4u,
            /*brdfLutSize=*/4u,
            /*sampleCount=*/16u);

        CHECK(ibl.irradiance.size == 2u);
        CHECK(ibl.irradiance.rgba8.size() == 2u * 2u * 4u * 6u);
        CHECK(ibl.prefilteredMips.size() == 3u);
        CHECK(ibl.prefilteredMips[0].size == 4u);
        CHECK(ibl.prefilteredMips[1].size == 2u);
        CHECK(ibl.prefilteredMips[2].size == 1u);
        CHECK(ibl.brdfLutSize == 4u);
        CHECK(ibl.brdfLutRgba8.size() == 4u * 4u * 4u);

        auto channelNear = [](uint8_t actual, uint8_t expected) {
            const int delta = static_cast<int>(actual)
                            - static_cast<int>(expected);
            return delta >= -2 && delta <= 2;
        };
        for (size_t pixel = 0; pixel < ibl.irradiance.rgba8.size() / 4u;
             ++pixel) {
            CHECK(channelNear(ibl.irradiance.rgba8[pixel * 4u], 64u));
            CHECK(channelNear(ibl.irradiance.rgba8[pixel * 4u + 1u], 128u));
            CHECK(channelNear(ibl.irradiance.rgba8[pixel * 4u + 2u], 192u));
            CHECK(ibl.irradiance.rgba8[pixel * 4u + 3u] == 255u);
        }
        for (const auto& mip : ibl.prefilteredMips) {
            for (size_t pixel = 0; pixel < mip.rgba8.size() / 4u; ++pixel) {
                CHECK(channelNear(mip.rgba8[pixel * 4u], 64u));
                CHECK(channelNear(mip.rgba8[pixel * 4u + 1u], 128u));
                CHECK(channelNear(mip.rgba8[pixel * 4u + 2u], 192u));
                CHECK(mip.rgba8[pixel * 4u + 3u] == 255u);
            }
        }
        for (size_t pixel = 0; pixel < ibl.brdfLutRgba8.size() / 4u;
             ++pixel) {
            CHECK(ibl.brdfLutRgba8[pixel * 4u + 3u] == 255u);
        }
}

TEST_CASE(invalid_source_returns_empty_set)
{
        const IblCpuSet ibl = buildIblCpuSet(nullptr, 0u, 2u, 4u, 4u, 4u);
        CHECK(ibl.irradiance.rgba8.empty());
        CHECK(ibl.prefilteredMips.empty());
        CHECK(ibl.brdfLutRgba8.empty());
}

TEST_SUITE_END
