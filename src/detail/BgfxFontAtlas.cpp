#include "detail/BgfxFontAtlas.h"

#include "detail/BGFXAdapter.h"

#include "AYFont/SystemFontDiscovery.h"
#include "AYUI/UnicodeText.h"

#include <AYCore/CoreUtility.h>

#include <bgfx/bgfx.h>

#include <cstdio>
#include <cwctype>
#include <filesystem>

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
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(path), ec);
}

std::wstring normalizedFamily(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

uint32_t nextTextCodePoint(const std::wstring& text, size_t& index)
{
    const uint32_t first = static_cast<uint32_t>(text[index++]);
    if constexpr (sizeof(wchar_t) == 2) {
        if (first >= 0xD800u && first <= 0xDBFFu && index < text.size()) {
            const uint32_t second = static_cast<uint32_t>(text[index]);
            if (second >= 0xDC00u && second <= 0xDFFFu) {
                ++index;
                return 0x10000u + ((first - 0xD800u) << 10u) + (second - 0xDC00u);
            }
        }
        if (first >= 0xD800u && first <= 0xDFFFu) return 0xFFFDu;
    }
    return first;
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
        // Prefer a CJK-capable face so TextInput / labels can show Chinese.
        // Segoe UI / Arial alone render missing glyphs as tofu (□□□□).
        L"C:\\Windows\\Fonts\\msyh.ttc",
        L"C:\\Windows\\Fonts\\msyhbd.ttc",
        L"C:\\Windows\\Fonts\\simsun.ttc",
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
        static const wchar_t* kPortableDefaults[] = {
            L"Noto Sans CJK SC", L"Noto Sans", L"DejaVu Sans",
            L"Liberation Sans", L"Arial"
        };
        for (const wchar_t* family : kPortableDefaults) {
            if (const ayt::font::SystemFontFace* face =
                    ayt::font::matchSystemFont(family, 400, false)) {
                if (tryRegisterFont(14, face->path.c_str())) {
                    registered = true;
                    break;
                }
            }
        }
    }

    if (!registered) {
        std::fprintf(stderr, "[BgfxFontAtlas] no UI font registered\n");
        return false;
    }

    // Seed the family → path table for getFontHandle(familyName, size).
    // Only families whose file actually exists are mapped (same existence
    // check tryRegisterFont uses); anything else falls back to the default
    // face at that size.
    static const struct {
        const wchar_t* family;
        const wchar_t* regular;
        const wchar_t* bold;
        const wchar_t* italic;
        const wchar_t* boldItalic;
    } kFamilyPaths[] = {
        {L"Microsoft YaHei", L"C:\\Windows\\Fonts\\msyh.ttc",
         L"C:\\Windows\\Fonts\\msyhbd.ttc", nullptr, nullptr},
        {L"Microsoft YaHei UI", L"C:\\Windows\\Fonts\\msyh.ttc",
         L"C:\\Windows\\Fonts\\msyhbd.ttc", nullptr, nullptr},
        {L"SimSun", L"C:\\Windows\\Fonts\\simsun.ttc", nullptr, nullptr, nullptr},
        {L"Segoe UI", L"C:\\Windows\\Fonts\\segoeui.ttf",
         L"C:\\Windows\\Fonts\\segoeuib.ttf",
         L"C:\\Windows\\Fonts\\segoeuii.ttf",
         L"C:\\Windows\\Fonts\\segoeuiz.ttf"},
        {L"Arial", L"C:\\Windows\\Fonts\\arial.ttf",
         L"C:\\Windows\\Fonts\\arialbd.ttf",
         L"C:\\Windows\\Fonts\\ariali.ttf",
         L"C:\\Windows\\Fonts\\arialbi.ttf"},
    };
    for (const auto& entry : kFamilyPaths) {
        if (!fileExists(entry.regular)) continue;
        FamilyFaces faces;
        faces.regular = entry.regular;
        if (fileExists(entry.bold)) faces.bold = entry.bold;
        if (fileExists(entry.italic)) faces.italic = entry.italic;
        if (fileExists(entry.boldItalic)) faces.boldItalic = entry.boldItalic;
        _familyFaces[normalizedFamily(entry.family)] = std::move(faces);
    }

    // Fill the same family table from platform font discovery. This is the
    // primary path on Linux/macOS and also exposes installed Windows fonts
    // beyond the small compatibility list above.
    for (const ayt::font::SystemFontFace& entry : ayt::font::discoverSystemFonts()) {
        FamilyFaces& faces = _familyFaces[normalizedFamily(entry.family)];
        if (entry.italic) {
            if (entry.weight >= 600) {
                if (faces.boldItalic.empty()) faces.boldItalic = entry.path;
            } else if (faces.italic.empty()) {
                faces.italic = entry.path;
            }
        } else if (entry.weight >= 600) {
            if (faces.bold.empty()) faces.bold = entry.path;
        } else if (faces.regular.empty()
                   || std::abs(entry.weight - 400) < 100) {
            faces.regular = entry.path;
        }
    }

    if (ensureGpuAtlas(acquireFont(14)) == kInvalidIdx) {
        std::fprintf(stderr, "[BgfxFontAtlas] atlas texture creation failed\n");
        return false;
    }
    return true;
}

void BgfxFontAtlas::shutdown(BGFXAdapter& adapter)
{
    for (auto& entry : _gpuAtlases) {
        if (entry.second.textureIdx != kInvalidIdx && adapter.isInitialized()) {
            adapter.destroy(bgfx::TextureHandle{entry.second.textureIdx});
        }
        entry.second.textureIdx = kInvalidIdx;
    }
    _gpuAtlases.clear();

    _shapersByFontId.clear();

    if (_fontManager != nullptr) {
        _fontManager->releaseAll();
        _fontManager.reset();
    }

    _fontsBySize.clear();
    _familyFaces.clear();
    _fontsByFaceRequest.clear();
    _fallbackFontsBySize.clear();
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

ayt::font::IFont* BgfxFontAtlas::acquireFont(const wchar_t* familyName, int pixelSize)
{
    return acquireFont(familyName, pixelSize, 400, false);
}

ayt::font::IFont* BgfxFontAtlas::acquireFont(const wchar_t* familyName,
                                              int pixelSize,
                                              int fontWeight,
                                              bool italic)
{
    if (_fontManager == nullptr || pixelSize < 8) return nullptr;
    const bool bold = fontWeight >= 600;
    std::wstring family = familyName != nullptr ? familyName : L"";
    if (family.empty() && !bold && !italic) return acquireFont(pixelSize);
    if (family.empty()) {
        if (_familyFaces.find(normalizedFamily(L"Microsoft YaHei UI")) != _familyFaces.end()) {
            family = L"Microsoft YaHei UI";
        } else {
            return acquireFont(pixelSize);
        }
    }

    const auto facesIt = _familyFaces.find(normalizedFamily(family));
    if (facesIt == _familyFaces.end()) return acquireFont(pixelSize);
    const FamilyFaces& faces = facesIt->second;
    const std::wstring* path = &faces.regular;
    if (bold && italic && !faces.boldItalic.empty()) path = &faces.boldItalic;
    else if (bold && !faces.bold.empty()) path = &faces.bold;
    else if (italic && !faces.italic.empty()) path = &faces.italic;
    if (path->empty()) path = &faces.regular;

    const int requestedWeight = std::clamp(fontWeight, 100, 900);
    const std::wstring requestKey = normalizedFamily(family) + L"|" + std::to_wstring(pixelSize)
        + L"|" + std::to_wstring(requestedWeight)
        + L"|" + std::to_wstring(italic ? 1 : 0);
    const auto existing = _fontsByFaceRequest.find(requestKey);
    if (existing != _fontsByFaceRequest.end()) {
        return _fontManager->getFont(existing->second);
    }

    wchar_t name[128] = {};
#if defined(_WIN32)
    swprintf_s(name, L"UI_%ls_%d_%d_%d", family.c_str(), pixelSize,
               requestedWeight, italic ? 1 : 0);
#else
    swprintf(name, sizeof(name) / sizeof(name[0]), L"UI_%ls_%d_%d_%d",
             family.c_str(), pixelSize, requestedWeight, italic ? 1 : 0);
#endif
    const ayt::font::FontHandle handle =
        _fontManager->registerFont(name, path->c_str(), pixelSize);
    if (!handle.isValid() || _fontManager->getFont(handle) == nullptr) {
        return acquireFont(pixelSize);
    }
    _fontManager->preloadFont(handle);
    if (ayt::font::IFont* loaded = _fontManager->getFont(handle);
        loaded != nullptr && loaded->hasVariationAxes()) {
        // CSS weight maps directly to the standard OpenType wght axis.
        // Fonts without that axis simply keep their default instance.
        (void)loaded->setVariationAxis(L"wght", static_cast<float>(requestedWeight));
    }
    _fontsByFaceRequest[requestKey] = handle;
    return _fontManager->getFont(handle);
}

ayt::font::IFont* BgfxFontAtlas::fontForHandle(ayt::font::FontHandle handle) const
{
    if (_fontManager == nullptr || !handle.isValid()) {
        return nullptr;
    }
    return _fontManager->getFont(handle);
}

ayt::font::FontHandle BgfxFontAtlas::handleForSize(int pixelSize) const
{
    const auto it = _fontsBySize.find(pixelSize);
    if (it == _fontsBySize.end()) {
        return ayt::font::FontHandle{};
    }
    return it->second;
}

ayt::font::IAYShaper* BgfxFontAtlas::acquireShaper(ayt::font::IFont* font)
{
    if (font == nullptr) {
        return nullptr;
    }
    const int id = font->getHandle().id;
    const auto it = _shapersByFontId.find(id);
    if (it != _shapersByFontId.end()) {
        return it->second.get();
    }

    ayt::font::IAYShaper* raw = ayt::font::createShaper(font);
    if (raw == nullptr) {
        return nullptr;
    }
    _shapersByFontId[id].reset(raw);
    return raw;
}

std::vector<ayt::font::ShapedGlyph> BgfxFontAtlas::shapeText(ayt::font::IFont* font,
                                                             const std::wstring& text)
{
    return shapeText(font, text, ayt::font::ShapingDirection::Auto, nullptr);
}

std::vector<ayt::font::ShapedGlyph> BgfxFontAtlas::shapeText(
    ayt::font::IFont* font, const std::wstring& text,
    ayt::font::ShapingDirection direction, const char* language)
{
    if (font == nullptr || text.empty()) {
        return {};
    }
    ayt::font::IAYShaper* shaper = acquireShaper(font);
    if (shaper == nullptr) {
        return {};
    }
    ayt::font::ShapingOptions options;
    options.direction = direction;
    options.language = language;
    return shaper->shapeWithOptions(text.c_str(), static_cast<int>(text.size()), options);
}

void BgfxFontAtlas::ensureFallbackFonts(int pixelSize, ayt::font::IFont* primary)
{
    if (_fontManager == nullptr || primary == nullptr
        || _fallbackFontsBySize.find(pixelSize) != _fallbackFontsBySize.end()) return;

    std::vector<const ayt::font::SystemFontFace*> candidates;
    for (const auto& face : ayt::font::discoverSystemFonts()) {
        // One upright regular/variable face per family is enough for glyph
        // coverage. The requested style remains on the primary face.
        if (!face.italic && face.weight >= 300 && face.weight <= 500) {
            candidates.push_back(&face);
        }
    }
    auto priority = [](const ayt::font::SystemFontFace* face) {
        const std::wstring name = normalizedFamily(face->family);
        if (face->color || name.find(L"emoji") != std::wstring::npos) return 0;
        if (name.find(L"symbol") != std::wstring::npos) return 1;
        if (name.find(L"cjk") != std::wstring::npos
            || name.find(L"yahei") != std::wstring::npos
            || name.find(L"simsun") != std::wstring::npos) return 2;
        if (name.find(L"noto sans") != std::wstring::npos) return 3;
        return 4;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const auto* a, const auto* b) {
        return priority(a) < priority(b);
    });

    std::vector<ayt::font::FontHandle> handles;
    std::unordered_set<std::wstring> families;
    for (const auto* face : candidates) {
        if (handles.size() >= 32u) break;
        const std::wstring familyKey = normalizedFamily(face->family);
        if (!families.insert(familyKey).second) continue;
        const std::wstring key = L"fallback|" + familyKey + L"|" + std::to_wstring(pixelSize);
        ayt::font::FontHandle handle;
        const auto existing = _fontsByFaceRequest.find(key);
        if (existing != _fontsByFaceRequest.end()) {
            handle = existing->second;
        } else {
            const std::wstring name = L"UI_Fallback_" + std::to_wstring(pixelSize)
                + L"_" + std::to_wstring(handles.size());
            handle = _fontManager->registerFont(name.c_str(), face->path.c_str(), pixelSize);
            if (handle.isValid()) _fontsByFaceRequest[key] = handle;
        }
        if (handle.isValid() && handle != primary->getHandle()) handles.push_back(handle);
    }
    _fallbackFontsBySize[pixelSize] = handles;
    _fontManager->setFallbackChain(primary->getHandle(), handles);
}

std::vector<BgfxFontAtlas::ShapedFontRun> BgfxFontAtlas::shapeTextWithFallback(
    ayt::font::IFont* primary, int pixelSize, const std::wstring& text,
    ayt::font::ShapingDirection direction, const char* language)
{
    std::vector<ShapedFontRun> result;
    if (primary == nullptr || text.empty() || _fontManager == nullptr) return result;
    ensureFallbackFonts(pixelSize, primary);
    const auto analysis = ayt::ui::analyzeUnicodeText(text, ayt::ui::TextDirection::Auto);
    for (const auto& cluster : analysis.clusters) {
        ayt::font::IFont* selected = primary;
        std::vector<ayt::font::IFont*> candidates{primary};
        const auto fallbackIt = _fallbackFontsBySize.find(pixelSize);
        if (fallbackIt != _fallbackFontsBySize.end()) {
            for (const auto handle : fallbackIt->second) {
                if (ayt::font::IFont* candidate = _fontManager->getFont(handle)) {
                    candidates.push_back(candidate);
                }
            }
        }
        auto covers = [&](ayt::font::IFont* candidate) {
            bool coversCluster = true;
            bool sawRequiredCodePoint = false;
            size_t cpIndex = cluster.textStart;
            while (cpIndex < cluster.textStart + cluster.textLength) {
                const uint32_t cp = nextTextCodePoint(text, cpIndex);
                // Joiners and variation selectors are shaping controls and
                // legitimately have no cmap glyph of their own.
                if (cp == 0x200Du || (cp >= 0xFE00u && cp <= 0xFE0Fu)
                    || (cp >= 0xE0100u && cp <= 0xE01EFu)) continue;
                sawRequiredCodePoint = true;
                if (!candidate->hasGlyph(cp)) { coversCluster = false; break; }
            }
            return coversCluster && sawRequiredCodePoint;
        };
        bool foundCoveringFace = false;
        for (ayt::font::IFont* candidate : candidates) {
            if (!covers(candidate)) continue;
            selected = candidate;
            foundCoveringFace = true;
            break;
        }
        // Uncommon scripts should not depend on falling inside a fixed
        // startup shortlist. Resolve the first matching installed family on
        // demand, then cache its handle in this size's fallback set.
        if (!foundCoveringFace) {
            for (const auto& face : ayt::font::discoverSystemFonts()) {
                if (face.italic || face.weight < 300 || face.weight > 500) continue;
                ayt::font::IFont* candidate = acquireFont(
                    face.family.c_str(), pixelSize, 400, false);
                if (candidate == nullptr || !covers(candidate)) continue;
                selected = candidate;
                foundCoveringFace = true;
                auto& handles = _fallbackFontsBySize[pixelSize];
                if (std::find(handles.begin(), handles.end(), candidate->getHandle()) == handles.end()) {
                    handles.push_back(candidate->getHandle());
                    _fontManager->setFallbackChain(primary->getHandle(), handles);
                }
                break;
            }
        }
        if (selected == nullptr) selected = primary;
        if (!result.empty() && result.back().font == selected
            && result.back().sourceStart + result.back().sourceLength == cluster.textStart) {
            result.back().sourceLength += cluster.textLength;
        } else {
            result.push_back({selected, cluster.textStart, cluster.textLength, {}});
        }
    }
    for (ShapedFontRun& run : result) {
        const std::wstring span = text.substr(run.sourceStart, run.sourceLength);
        run.glyphs = shapeText(run.font, span, direction, language);
        for (auto& glyph : run.glyphs) glyph.charIndex += static_cast<uint32_t>(run.sourceStart);
    }
    result.erase(std::remove_if(result.begin(), result.end(), [](const ShapedFontRun& run) {
        return run.glyphs.empty();
    }), result.end());
    if (direction == ayt::font::ShapingDirection::RightToLeft
        || (direction == ayt::font::ShapingDirection::Auto
            && analysis.baseRightToLeft)) {
        std::reverse(result.begin(), result.end());
    }
    return result;
}

void BgfxFontAtlas::prepareShapedGlyphs(ayt::font::IFont* font, int pixelSize,
                                        const std::vector<ayt::font::ShapedGlyph>& shaped)
{
    if (font == nullptr) {
        return;
    }
    for (const ayt::font::ShapedGlyph& sg : shaped) {
        font->getGlyphByIndex(sg.glyphIndex);
        FontGpuAtlas& atlas = _gpuAtlases[font->getHandle().id];
        if (atlas.knownGlyphs.insert(sg.glyphIndex).second) {
            markAtlasDirty(font);
        }
    }
}

void BgfxFontAtlas::markAtlasDirty(ayt::font::IFont* font)
{
    if (font != nullptr) _gpuAtlases[font->getHandle().id].dirty = true;
}

void BgfxFontAtlas::prepareGlyphs(ayt::font::IFont* font, int pixelSize, const std::wstring& text)
{
    if (font == nullptr) {
        return;
    }

    for (size_t index = 0; index < text.size();) {
        const uint32_t codepoint = nextTextCodePoint(text, index);
        font->getGlyph(codepoint);
        FontGpuAtlas& atlas = _gpuAtlases[font->getHandle().id];
        if (atlas.knownGlyphs.insert(codepoint).second) {
            markAtlasDirty(font);
        }
    }
}

float BgfxFontAtlas::measureShapedWidth(const std::vector<ayt::font::ShapedGlyph>& shaped) const
{
    float width = 0.0f;
    for (const ayt::font::ShapedGlyph& sg : shaped) {
        width += static_cast<float>(sg.xAdvance) / 64.0f;
    }
    return width;
}

uint16_t BgfxFontAtlas::ensureGpuAtlas(ayt::font::IFont* font)
{
    if (font == nullptr) return kInvalidIdx;
    FontGpuAtlas& atlas = _gpuAtlases[font->getHandle().id];
    if (atlas.textureIdx != kInvalidIdx) return atlas.textureIdx;
    const bgfx::TextureHandle handle = bgfx::createTexture2D(
        static_cast<uint16_t>(kAtlasWidth), static_cast<uint16_t>(kAtlasHeight), false, 1,
        bgfx::TextureFormat::BGRA8,
        BGFX_TEXTURE_NONE | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT,
        nullptr);
    if (!bgfx::isValid(handle)) return kInvalidIdx;
    atlas.textureIdx = handle.idx;
    atlas.dirty = true;
    atlas.bgraScratch.resize(
        static_cast<size_t>(kAtlasWidth) * static_cast<size_t>(kAtlasHeight) * 4u);
    return atlas.textureIdx;
}

uint16_t BgfxFontAtlas::atlasTextureIdx(ayt::font::IFont* font) const
{
    if (font == nullptr) return kInvalidIdx;
    const auto it = _gpuAtlases.find(font->getHandle().id);
    return it == _gpuAtlases.end() ? kInvalidIdx : it->second.textureIdx;
}

bool BgfxFontAtlas::isAtlasDirty(ayt::font::IFont* font) const
{
    if (font == nullptr) return false;
    const auto it = _gpuAtlases.find(font->getHandle().id);
    return it == _gpuAtlases.end() || it->second.dirty;
}

void BgfxFontAtlas::syncAtlasToGpu(ayt::font::IFont* font)
{
    if (font == nullptr || ensureGpuAtlas(font) == kInvalidIdx) return;
    FontGpuAtlas& atlas = _gpuAtlases[font->getHandle().id];
    if (!atlas.dirty) return;

    const uint8_t* pixels = static_cast<const uint8_t*>(font->getAtlasTexture());
    if (pixels == nullptr) {
        return;
    }

    const size_t pixelCount = static_cast<size_t>(kAtlasWidth) * static_cast<size_t>(kAtlasHeight);
    if (atlas.bgraScratch.size() < pixelCount * 4u) {
        atlas.bgraScratch.resize(pixelCount * 4u);
    }

    if (font->getAtlasBytesPerPixel() == 4) {
        std::copy(pixels, pixels + pixelCount * 4u, atlas.bgraScratch.begin());
    } else {
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t alpha = pixels[i];
            const size_t dst = i * 4u;
            atlas.bgraScratch[dst + 0] = 255;
            atlas.bgraScratch[dst + 1] = 255;
            atlas.bgraScratch[dst + 2] = 255;
            atlas.bgraScratch[dst + 3] = alpha;
        }
    }

    const bgfx::Memory* mem =
        bgfx::copy(atlas.bgraScratch.data(), static_cast<uint32_t>(atlas.bgraScratch.size()));
    bgfx::updateTexture2D(bgfx::TextureHandle{atlas.textureIdx}, 0, 0, 0, 0,
                          static_cast<uint16_t>(kAtlasWidth), static_cast<uint16_t>(kAtlasHeight),
                          mem);
    atlas.dirty = false;
}

} // namespace ayt::render::detail
