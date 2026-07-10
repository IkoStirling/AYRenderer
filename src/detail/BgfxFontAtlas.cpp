#include "detail/BgfxFontAtlas.h"

#include "detail/BGFXAdapter.h"

#include <AYCoreUtility.h>

#include <bgfx/bgfx.h>

#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <Windows.h>
#endif

namespace ayt::render::detail {

namespace {

constexpr uint16_t kInvalidIdx = UINT16_MAX;

bool fileExists(const wchar_t* path)
{
#if defined(_WIN32)
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    const DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    AYUNREFERENCED_PARAM(path);
    return false;
#endif
}

} // namespace

BgfxFontAtlas::BgfxFontAtlas() = default;

BgfxFontAtlas::~BgfxFontAtlas()
{
    if (_adapter != nullptr) {
        shutdown(*_adapter);
    }
}

bool BgfxFontAtlas::initialize(BGFXAdapter& adapter)
{
    if (_fontManager != nullptr) {
        return true;
    }

    _adapter = &adapter;
    _fontManager.reset(ayt::font::createFontManager());
    if (_fontManager == nullptr) {
        std::fprintf(stderr, "[BgfxFontAtlas] createFontManager failed\n");
        return false;
    }

    static const wchar_t* kCandidates[] = {
        L"C:\\Windows\\Fonts\\segoeui.ttf",
        L"C:\\Windows\\Fonts\\arial.ttf",
    };

    bool registered = false;
    for (const wchar_t* path : kCandidates) {
        if (tryRegisterFont(14, path)) {
            registered = true;
            break;
        }
    }

    if (!registered) {
        std::fprintf(stderr, "[BgfxFontAtlas] no UI font registered\n");
        return false;
    }

    _bgraScratch.resize(static_cast<size_t>(kAtlasWidth) * static_cast<size_t>(kAtlasHeight) * 4u);

    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(kAtlasWidth), static_cast<uint16_t>(kAtlasHeight), false, 1,
        bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        nullptr);
    if (!bgfx::isValid(handle)) {
        std::fprintf(stderr, "[BgfxFontAtlas] atlas texture creation failed\n");
        return false;
    }

    _atlasTextureIdx = handle.idx;
    _atlasDirty      = false;
    return true;
}

void BgfxFontAtlas::shutdown(BGFXAdapter& adapter)
{
    if (adapter.isInitialized() && _atlasTextureIdx != kInvalidIdx) {
        adapter.destroy(bgfx::TextureHandle{_atlasTextureIdx});
        _atlasTextureIdx = kInvalidIdx;
    } else if (_atlasTextureIdx != kInvalidIdx) {
        _atlasTextureIdx = kInvalidIdx;
    }

    if (_fontManager != nullptr) {
        _fontManager->releaseAll();
        _fontManager.reset();
    }

    _fontsBySize.clear();
    _bgraScratch.clear();
    _atlasDirty = true;
    _knownGlyphs.clear();
    _adapter    = nullptr;
}

bool BgfxFontAtlas::tryRegisterFont(int pixelSize, const wchar_t* path)
{
    if (!fileExists(path)) {
        return false;
    }

    wchar_t name[32] = {};
#if defined(_WIN32)
    swprintf_s(name, L"UI_%d", pixelSize);
#else
    swprintf(name, sizeof(name) / sizeof(name[0]), L"UI_%d", pixelSize);
#endif
    const ayt::font::FontHandle handle = _fontManager->registerFont(name, path, pixelSize);
    if (!handle.isValid()) {
        return false;
    }

    _fontsBySize[pixelSize] = handle;
    _fontManager->preloadFont(handle);
    return _fontManager->getFont(handle) != nullptr;
}

ayt::font::IFont* BgfxFontAtlas::registerFontForSize(int pixelSize)
{
    const auto existing = _fontsBySize.find(pixelSize);
    if (existing != _fontsBySize.end()) {
        return _fontManager->getFont(existing->second);
    }

    const auto baseIt = _fontsBySize.find(14);
    if (baseIt == _fontsBySize.end()) {
        return nullptr;
    }

    std::wstring path;
    for (const ayt::font::FontInfo& info : _fontManager->getFontInfoList()) {
        if (info.baseSize == 14) {
            path = info.path;
            break;
        }
    }

    if (path.empty() || !tryRegisterFont(pixelSize, path.c_str())) {
        return _fontManager->getFont(baseIt->second);
    }

    return _fontManager->getFont(_fontsBySize[pixelSize]);
}

ayt::font::IFont* BgfxFontAtlas::acquireFont(int pixelSize)
{
    if (_fontManager == nullptr || pixelSize < 8) {
        return nullptr;
    }

    const auto it = _fontsBySize.find(pixelSize);
    if (it != _fontsBySize.end()) {
        return _fontManager->getFont(it->second);
    }

    return registerFontForSize(pixelSize);
}

void BgfxFontAtlas::markAtlasDirty()
{
    _atlasDirty = true;
}

void BgfxFontAtlas::prepareGlyphs(ayt::font::IFont* font, int pixelSize, const std::wstring& text)
{
    if (font == nullptr) {
        return;
    }

    for (wchar_t ch : text) {
        const uint32_t codepoint = static_cast<uint32_t>(ch);
        font->getGlyph(codepoint);
        const uint64_t key =
            (static_cast<uint64_t>(static_cast<uint32_t>(pixelSize)) << 32) | codepoint;
        if (_knownGlyphs.insert(key).second) {
            markAtlasDirty();
        }
    }
}

void BgfxFontAtlas::syncAtlasToGpu(ayt::font::IFont* font)
{
    if (font == nullptr || _atlasTextureIdx == kInvalidIdx || !_atlasDirty) {
        return;
    }

    const uint8_t* gray = static_cast<const uint8_t*>(font->getAtlasTexture());
    if (gray == nullptr) {
        return;
    }

    const size_t pixelCount = static_cast<size_t>(kAtlasWidth) * static_cast<size_t>(kAtlasHeight);
    if (_bgraScratch.size() < pixelCount * 4u) {
        _bgraScratch.resize(pixelCount * 4u);
    }

    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t alpha   = gray[i];
        const size_t  dst     = i * 4u;
        _bgraScratch[dst + 0] = 255;
        _bgraScratch[dst + 1] = 255;
        _bgraScratch[dst + 2] = 255;
        _bgraScratch[dst + 3] = alpha;
    }

    const bgfx::Memory* mem =
        bgfx::copy(_bgraScratch.data(), static_cast<uint32_t>(_bgraScratch.size()));
    bgfx::updateTexture2D(bgfx::TextureHandle{_atlasTextureIdx}, 0, 0, 0, 0,
                          static_cast<uint16_t>(kAtlasWidth), static_cast<uint16_t>(kAtlasHeight),
                          mem);
    _atlasDirty = false;
}

} // namespace ayt::render::detail
