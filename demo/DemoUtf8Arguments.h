#pragma once
#include <filesystem>
#include <string>
#include <vector>
namespace ayt::render::demo {
// Windows' narrow argv uses the process ACP. Convert the original UTF-16
// arguments explicitly so renderer/file APIs continue receiving UTF-8 paths.
struct Utf8Arguments {
    std::vector<std::string> storage;
    std::vector<char*> pointers;
    Utf8Arguments(int count,wchar_t** arguments) {
        storage.reserve(size_t(count));pointers.reserve(size_t(count));
        for(int i=0;i<count;++i) {
            const auto text=std::filesystem::path(arguments[i]).u8string();
            storage.emplace_back(text.begin(),text.end());
        }
        for(auto& text:storage)pointers.push_back(text.data());
    }
};
}
