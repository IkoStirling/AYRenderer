#pragma once

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace ayt::render::detail
{

class BGFXAdapter;
struct GpuTexture;

struct IblCpuCube {
    uint16_t size = 0;
    std::vector<uint8_t> rgba8;
};

struct IblCpuSet {
    IblCpuCube irradiance;
    std::vector<IblCpuCube> prefilteredMips;
    uint16_t brdfLutSize = 0;
    std::vector<uint8_t> brdfLutRgba8;
};

// CPU precomputation is deliberately source-change driven. Production uses
// the defaults; tests can request tiny outputs and fewer samples.
IblCpuSet buildIblCpuSet(const uint8_t* sourceFacesRgba8,
                         uint16_t sourceSize,
                         uint16_t irradianceSize = 16,
                         uint16_t prefilteredSize = 64,
                         uint16_t brdfLutSize = 128,
                         uint32_t sampleCount = 64);

class IblResources {
public:
    bool ensure(BGFXAdapter& adapter, uint64_t sourceTextureId,
                const GpuTexture& source);
    void destroy(BGFXAdapter& adapter);

    bool isReadyFor(uint64_t sourceTextureId) const noexcept;
    bgfx::TextureHandle irradiance() const noexcept { return _irradiance; }
    bgfx::TextureHandle prefilteredSpecular() const noexcept {
        return _prefilteredSpecular;
    }
    bgfx::TextureHandle brdfLut() const noexcept { return _brdfLut; }
    float maxSpecularLod() const noexcept { return _maxSpecularLod; }

private:
    bgfx::TextureHandle _irradiance = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle _prefilteredSpecular = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle _brdfLut = BGFX_INVALID_HANDLE;
    uint64_t _sourceTextureId = 0;
    uint64_t _failedSourceTextureId = 0;
    float _maxSpecularLod = 0.0f;
};

} // namespace ayt::render::detail
