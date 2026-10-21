#pragma once
#include <cstddef>
#include <string_view>

namespace ayt::render::detail {
struct UiTextParagraphRange { size_t begin = 0, end = 0; };

// Visit logical paragraphs before shaping, without allocating or altering UTF
// code units. CRLF is one boundary; empty and trailing paragraphs are retained.
// The legacy single-line route deliberately visits the complete span once.
template<class Visitor>
void forEachUiTextParagraph(std::wstring_view text, bool multiline, Visitor&& visit)
{
    if (!multiline) { visit(UiTextParagraphRange{0, text.size()}); return; }
    size_t begin = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        const auto value = text[index];
        if (value != L'\n' && value != L'\r' && value != L'\u2028' && value != L'\u2029') continue;
        visit(UiTextParagraphRange{begin, index});
        if (value == L'\r' && index + 1 < text.size() && text[index + 1] == L'\n') ++index;
        begin = index + 1;
    }
    visit(UiTextParagraphRange{begin, text.size()});
}
} // namespace ayt::render::detail
