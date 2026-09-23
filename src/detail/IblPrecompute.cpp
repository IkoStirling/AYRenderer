#include "detail/IblPrecompute.h"

#include "detail/BGFXAdapter.h"
#include "detail/GpuResources.h"

#include <algorithm>
#include <cmath>

namespace ayt::render::detail
{
namespace
{

constexpr float kPi = 3.14159265358979323846f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
float dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
Vec3 normalize(Vec3 v) {
    const float length = std::sqrt(std::max(dot(v, v), 1.0e-12f));
    return v * (1.0f / length);
}

float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

void basis(Vec3 n, Vec3& tangent, Vec3& bitangent)
{
    const Vec3 up = std::fabs(n.z) < 0.999f ? Vec3{0,0,1} : Vec3{1,0,0};
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

Vec3 faceDirection(uint32_t face, float u, float v)
{
    switch (face) {
    case 0: return normalize({ 1.0f, -v, -u});
    case 1: return normalize({-1.0f, -v,  u});
    case 2: return normalize({ u,  1.0f,  v});
    case 3: return normalize({ u, -1.0f, -v});
    case 4: return normalize({ u, -v,  1.0f});
    default:return normalize({-u, -v, -1.0f});
    }
}

Vec3 sourceSample(const uint8_t* pixels, uint16_t size, Vec3 d)
{
    d = normalize(d);
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    uint32_t face = 0;
    float u = 0.0f, v = 0.0f;
    if (ax >= ay && ax >= az) {
        if (d.x >= 0.0f) { face = 0; u = -d.z/ax; v = -d.y/ax; }
        else             { face = 1; u =  d.z/ax; v = -d.y/ax; }
    } else if (ay >= az) {
        if (d.y >= 0.0f) { face = 2; u = d.x/ay; v =  d.z/ay; }
        else             { face = 3; u = d.x/ay; v = -d.z/ay; }
    } else {
        if (d.z >= 0.0f) { face = 4; u =  d.x/az; v = -d.y/az; }
        else             { face = 5; u = -d.x/az; v = -d.y/az; }
    }
    const float fx = std::clamp((u * 0.5f + 0.5f) * (size - 1u), 0.0f,
                                static_cast<float>(size - 1u));
    const float fy = std::clamp((v * 0.5f + 0.5f) * (size - 1u), 0.0f,
                                static_cast<float>(size - 1u));
    const uint32_t x0 = static_cast<uint32_t>(fx);
    const uint32_t y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min<uint32_t>(x0 + 1u, size - 1u);
    const uint32_t y1 = std::min<uint32_t>(y0 + 1u, size - 1u);
    const float tx = fx - x0, ty = fy - y0;
    auto texel = [&](uint32_t x, uint32_t y) {
        const size_t index = ((static_cast<size_t>(face) * size * size)
                              + static_cast<size_t>(y) * size + x) * 4u;
        return Vec3{pixels[index] / 255.0f, pixels[index+1] / 255.0f,
                    pixels[index+2] / 255.0f};
    };
    const Vec3 a = texel(x0, y0) * (1.0f - tx) + texel(x1, y0) * tx;
    const Vec3 b = texel(x0, y1) * (1.0f - tx) + texel(x1, y1) * tx;
    return a * (1.0f - ty) + b * ty;
}

uint8_t toByte(float value)
{
    return static_cast<uint8_t>(std::lround(
        std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

void writePixel(std::vector<uint8_t>& out, size_t pixel, Vec3 value)
{
    out[pixel*4u+0u] = toByte(value.x);
    out[pixel*4u+1u] = toByte(value.y);
    out[pixel*4u+2u] = toByte(value.z);
    out[pixel*4u+3u] = 255u;
}

Vec3 importanceGgx(float xiX, float xiY, Vec3 n, float roughness)
{
    const float a = std::max(roughness * roughness, 0.001f);
    const float a2 = a * a;
    const float phi = 2.0f * kPi * xiX;
    const float cosTheta = std::sqrt(
        (1.0f - xiY) / std::max(1.0f + (a2 - 1.0f) * xiY, 1.0e-6f));
    const float sinTheta = std::sqrt(std::max(1.0f - cosTheta*cosTheta, 0.0f));
    Vec3 t, b;
    basis(n, t, b);
    return normalize(t * (std::cos(phi) * sinTheta)
                   + b * (std::sin(phi) * sinTheta) + n * cosTheta);
}

float geometrySchlick(float ndot, float roughness)
{
    const float k = roughness * roughness * 0.5f;
    return ndot / std::max(ndot * (1.0f - k) + k, 1.0e-6f);
}

} // namespace

IblCpuSet buildIblCpuSet(const uint8_t* sourceFacesRgba8,
                         uint16_t sourceSize,
                         uint16_t irradianceSize,
                         uint16_t prefilteredSize,
                         uint16_t brdfLutSize,
                         uint32_t sampleCount)
{
    IblCpuSet result;
    if (sourceFacesRgba8 == nullptr || sourceSize == 0u) {
        return result;
    }
    irradianceSize = std::max<uint16_t>(irradianceSize, 1u);
    prefilteredSize = std::max<uint16_t>(prefilteredSize, 1u);
    brdfLutSize = std::max<uint16_t>(brdfLutSize, 1u);
    sampleCount = std::max(sampleCount, 1u);

    result.irradiance.size = irradianceSize;
    result.irradiance.rgba8.resize(
        static_cast<size_t>(irradianceSize) * irradianceSize * 4u * 6u);
    for (uint32_t face = 0; face < 6u; ++face) {
        for (uint32_t y = 0; y < irradianceSize; ++y) {
            for (uint32_t x = 0; x < irradianceSize; ++x) {
                const float u = (x + 0.5f) * 2.0f / irradianceSize - 1.0f;
                const float v = (y + 0.5f) * 2.0f / irradianceSize - 1.0f;
                const Vec3 n = faceDirection(face, u, v);
                Vec3 t, b;
                basis(n, t, b);
                Vec3 sum{};
                for (uint32_t i = 0; i < sampleCount; ++i) {
                    const float xiX = (i + 0.5f) / sampleCount;
                    const float xiY = radicalInverse(i);
                    const float r = std::sqrt(xiY);
                    const float phi = 2.0f * kPi * xiX;
                    const Vec3 l = normalize(t * (r * std::cos(phi))
                        + b * (r * std::sin(phi))
                        + n * std::sqrt(std::max(1.0f - xiY, 0.0f)));
                    sum += sourceSample(sourceFacesRgba8, sourceSize, l);
                }
                const size_t pixel = static_cast<size_t>(face)*irradianceSize*irradianceSize
                    + static_cast<size_t>(y)*irradianceSize + x;
                writePixel(result.irradiance.rgba8, pixel,
                           sum * (1.0f / sampleCount));
            }
        }
    }

    uint16_t mipSize = prefilteredSize;
    uint32_t mipCount = 1u;
    for (uint16_t s = mipSize; s > 1u; s = std::max<uint16_t>(s / 2u, 1u)) {
        ++mipCount;
    }
    result.prefilteredMips.reserve(mipCount);
    for (uint32_t mip = 0; mip < mipCount; ++mip) {
        IblCpuCube cube;
        cube.size = mipSize;
        cube.rgba8.resize(static_cast<size_t>(mipSize)*mipSize*4u*6u);
        const float roughness = mipCount > 1u
            ? static_cast<float>(mip) / static_cast<float>(mipCount - 1u)
            : 0.0f;
        for (uint32_t face = 0; face < 6u; ++face) {
            for (uint32_t y = 0; y < mipSize; ++y) {
                for (uint32_t x = 0; x < mipSize; ++x) {
                    const float u = (x + 0.5f) * 2.0f / mipSize - 1.0f;
                    const float v = (y + 0.5f) * 2.0f / mipSize - 1.0f;
                    const Vec3 n = faceDirection(face, u, v);
                    const Vec3 view = n;
                    Vec3 sum{};
                    float weight = 0.0f;
                    for (uint32_t i = 0; i < sampleCount; ++i) {
                        const Vec3 h = importanceGgx(
                            (i + 0.5f) / sampleCount, radicalInverse(i),
                            n, roughness);
                        const Vec3 l = normalize(h * (2.0f * dot(view, h)) - view);
                        const float ndotl = std::max(dot(n, l), 0.0f);
                        if (ndotl > 0.0f) {
                            sum += sourceSample(sourceFacesRgba8, sourceSize, l) * ndotl;
                            weight += ndotl;
                        }
                    }
                    const size_t pixel = static_cast<size_t>(face)*mipSize*mipSize
                        + static_cast<size_t>(y)*mipSize + x;
                    writePixel(cube.rgba8, pixel,
                               sum * (1.0f / std::max(weight, 1.0e-6f)));
                }
            }
        }
        result.prefilteredMips.push_back(std::move(cube));
        mipSize = std::max<uint16_t>(mipSize / 2u, 1u);
    }

    result.brdfLutSize = brdfLutSize;
    result.brdfLutRgba8.resize(
        static_cast<size_t>(brdfLutSize) * brdfLutSize * 4u);
    for (uint32_t y = 0; y < brdfLutSize; ++y) {
        const float roughness = (y + 0.5f) / brdfLutSize;
        for (uint32_t x = 0; x < brdfLutSize; ++x) {
            const float ndotv = std::max((x + 0.5f) / brdfLutSize, 0.001f);
            const Vec3 n{0,0,1};
            const Vec3 view{std::sqrt(std::max(1.0f-ndotv*ndotv, 0.0f)), 0, ndotv};
            float a = 0.0f, bValue = 0.0f;
            for (uint32_t i = 0; i < sampleCount; ++i) {
                const Vec3 h = importanceGgx(
                    (i + 0.5f) / sampleCount, radicalInverse(i), n, roughness);
                const Vec3 l = normalize(h * (2.0f * dot(view, h)) - view);
                const float ndotl = std::max(l.z, 0.0f);
                const float ndoth = std::max(h.z, 0.0f);
                const float vdoth = std::max(dot(view, h), 0.0f);
                if (ndotl > 0.0f) {
                    const float g = geometrySchlick(ndotv, roughness)
                        * geometrySchlick(ndotl, roughness);
                    const float gVis = g * vdoth
                        / std::max(ndoth * ndotv, 1.0e-6f);
                    const float fc = std::pow(1.0f - vdoth, 5.0f);
                    a += (1.0f - fc) * gVis;
                    bValue += fc * gVis;
                }
            }
            const size_t pixel = static_cast<size_t>(y)*brdfLutSize + x;
            result.brdfLutRgba8[pixel*4u+0u] = toByte(a / sampleCount);
            result.brdfLutRgba8[pixel*4u+1u] = toByte(bValue / sampleCount);
            result.brdfLutRgba8[pixel*4u+2u] = 0u;
            result.brdfLutRgba8[pixel*4u+3u] = 255u;
        }
    }
    return result;
}

bool IblResources::ensure(BGFXAdapter& adapter, uint64_t sourceTextureId,
                          const GpuTexture& source)
{
    if (isReadyFor(sourceTextureId)) {
        return true;
    }
    if (sourceTextureId != 0u && _failedSourceTextureId == sourceTextureId) {
        return false;
    }
    destroy(adapter);
    const size_t requiredBytes = static_cast<size_t>(source.width)
        * source.height * 4u * 6u;
    if (!source.cube || source.width == 0u || source.width != source.height
        || source.cpuRgba8.size() < requiredBytes) {
        _failedSourceTextureId = sourceTextureId;
        return false;
    }
    const uint16_t prefilterSize = std::min<uint16_t>(source.width, 64u);
    const IblCpuSet cpu = buildIblCpuSet(
        source.cpuRgba8.data(), source.width, 16u, prefilterSize, 128u, 64u);
    // bgfx's linear min/mag/mip mode is encoded by the absence of the
    // corresponding POINT flags.
    const uint64_t cubeFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP
        | BGFX_SAMPLER_W_CLAMP;
    _irradiance = adapter.createTextureCube(
        cpu.irradiance.size, cpu.irradiance.rgba8.data(), cubeFlags);
    _prefilteredSpecular = adapter.createMutableTextureCube(
        prefilterSize, true, bgfx::TextureFormat::RGBA8, cubeFlags);
    if (BGFXAdapter::isValid(_prefilteredSpecular)) {
        for (uint8_t mip = 0; mip < cpu.prefilteredMips.size(); ++mip) {
            const IblCpuCube& cube = cpu.prefilteredMips[mip];
            const uint32_t faceBytes = static_cast<uint32_t>(cube.size)
                * cube.size * 4u;
            for (uint8_t face = 0; face < 6u; ++face) {
                adapter.updateTextureCube(
                    _prefilteredSpecular, face, mip, cube.size,
                    cube.rgba8.data() + static_cast<size_t>(face) * faceBytes,
                    faceBytes);
            }
        }
    }
    const uint64_t lutFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    _brdfLut = adapter.createTexture2DFromData(
        cpu.brdfLutSize, cpu.brdfLutSize, bgfx::TextureFormat::RGBA8,
        cpu.brdfLutRgba8.data(),
        static_cast<uint32_t>(cpu.brdfLutRgba8.size()), lutFlags);
    if (!BGFXAdapter::isValid(_irradiance)
        || !BGFXAdapter::isValid(_prefilteredSpecular)
        || !BGFXAdapter::isValid(_brdfLut)) {
        destroy(adapter);
        _failedSourceTextureId = sourceTextureId;
        return false;
    }
    _sourceTextureId = sourceTextureId;
    _failedSourceTextureId = 0u;
    _maxSpecularLod = static_cast<float>(cpu.prefilteredMips.size() - 1u);
    return true;
}

bool IblResources::isReadyFor(uint64_t sourceTextureId) const noexcept
{
    return sourceTextureId != 0u && _sourceTextureId == sourceTextureId
        && BGFXAdapter::isValid(_irradiance)
        && BGFXAdapter::isValid(_prefilteredSpecular)
        && BGFXAdapter::isValid(_brdfLut);
}

void IblResources::destroy(BGFXAdapter& adapter)
{
    adapter.destroy(_irradiance);
    adapter.destroy(_prefilteredSpecular);
    adapter.destroy(_brdfLut);
    _irradiance = BGFX_INVALID_HANDLE;
    _prefilteredSpecular = BGFX_INVALID_HANDLE;
    _brdfLut = BGFX_INVALID_HANDLE;
    _sourceTextureId = 0u;
    _failedSourceTextureId = 0u;
    _maxSpecularLod = 0.0f;
}

} // namespace ayt::render::detail
