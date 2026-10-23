#pragma once
#include <cstdint>
#include <span>
#include <string_view>
namespace ayt::render::detail::pointgpu {
inline uint64_t binaryHash(std::span<const uint8_t> bytes) noexcept {
    uint64_t hash=14695981039346656037ull;
    for(auto byte:bytes) { hash^=byte; hash*=1099511628211ull; }
    return hash;
}
inline bool validBinary(std::span<const uint8_t> bytes,std::string_view stage,uint64_t hash) noexcept {
    return bytes.size()>=8 && bytes.size()<=16u*1024u*1024u && stage.size()==3
        && bytes[0]==stage[0] && bytes[1]==stage[1] && bytes[2]==stage[2]
        && binaryHash(bytes)==hash;
}
}
