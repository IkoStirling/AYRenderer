#pragma once

#include "IAYFontManager.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ayt::render::detail {

class BGFXAdapter;

// Uploads FreeType grayscale glyph atlas (512x512) to a persistent bgfx texture.
class BgfxFontAtlas {
public:
    static constexpr int kAtlasWidth  = 512;
    static constexpr int kAtlasHeight = 512;

    BgfxFontAtlas();
    ~BgfxFontAtlas();

    BgfxFontAtlas(const BgfxFontAtlas&) = delete;
    BgfxFontAtlas& operator=(const BgfxFontAtlas&) = delete;

    bool initialize(BGFXAdapter& adapter);
    void shutdown(BGFXAdapter& adapter);

    ayt::font::IFont* acquireFont(int pixelSize);
    uint16_t          atlasTextureIdx() const { return _atlasTextureIdx; }

    void markAtlasDirty();
    bool isAtlasDirty() const { return _atlasDirty; }
    void syncAtlasToGpu(ayt::font::IFont* font);

private:
    ayt::font::IFont* registerFontForSize(int pixelSize);
    bool              tryRegisterFont(int pixelSize, const wchar_t* path);

    std::unique_ptr<ayt::font::IFontManager> _fontManager;
    std::unordered_map<int, ayt::font::FontHandle> _fontsBySize;

    uint16_t             _atlasTextureIdx = UINT16_MAX;
    bool                 _atlasDirty      = true;
    std::vector<uint8_t> _bgraScratch;
    BGFXAdapter*         _adapter         = nullptr;
};

} // namespace ayt::render::detail
