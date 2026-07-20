#pragma once

#include "IAYFontManager.h"
#include "IAYShaper.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    ayt::font::IFont* fontForHandle(ayt::font::FontHandle handle) const;
    ayt::font::FontHandle handleForSize(int pixelSize) const;
    uint16_t              atlasTextureIdx() const { return _atlasTextureIdx; }

    // Shape UTF-16 text with HarfBuzz (falls back to empty on failure).
    std::vector<ayt::font::ShapedGlyph> shapeText(ayt::font::IFont* font,
                                                  const std::wstring& text);

    // Rasterize shaped glyphs; marks atlas dirty when a new glyph index appears.
    void prepareShapedGlyphs(ayt::font::IFont* font, int pixelSize,
                             const std::vector<ayt::font::ShapedGlyph>& shaped);

    // Legacy char-path warm-up (still used if shaping fails).
    void prepareGlyphs(ayt::font::IFont* font, int pixelSize, const std::wstring& text);

    float measureShapedWidth(const std::vector<ayt::font::ShapedGlyph>& shaped) const;

    void markAtlasDirty();
    bool isAtlasDirty() const { return _atlasDirty; }
    void syncAtlasToGpu(ayt::font::IFont* font);

private:
    ayt::font::IFont* registerFontForSize(int pixelSize);
    bool              tryRegisterFont(int pixelSize, const wchar_t* path);
    ayt::font::IAYShaper* acquireShaper(ayt::font::IFont* font);

    std::unique_ptr<ayt::font::IFontManager> _fontManager;
    std::unordered_map<int, ayt::font::FontHandle> _fontsBySize;
    std::unordered_map<int, std::unique_ptr<ayt::font::IAYShaper>> _shapersByFontId;

    uint16_t             _atlasTextureIdx = UINT16_MAX;
    bool                 _atlasDirty      = true;
    std::vector<uint8_t> _bgraScratch;
    BGFXAdapter*         _adapter         = nullptr;
    std::unordered_set<uint64_t> _knownGlyphs;
};

} // namespace ayt::render::detail
