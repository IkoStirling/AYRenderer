#pragma once
#include <bgfx/bgfx.h>
#include <cstdint>

namespace ayt::render::detail {
const char* textureStorageFormatName(bgfx::TextureFormat::Enum format) noexcept;
// Pure logical block accounting, independent of device admission or allocation.
// The output is unchanged on invalid dimensions/format/levels or uint64 overflow.
bool textureLogicalStorageBytes(uint32_t width, uint32_t height, uint32_t mipCount,
    bool cube, bgfx::TextureFormat::Enum format, uint64_t& output) noexcept;
}
