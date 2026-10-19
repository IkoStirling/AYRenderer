#include "AYRenderer/ColorStylizationLook.h"
#include "detail/ColorStylizationFields.h"
#include <AYIO/File.h>
#include <nlohmann/json.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace ayt::render {
namespace {
using Json=nlohmann::json;
constexpr size_t kMaxBytes=64*1024;
Json encodeColor(StylizationColor c) { return Json::array({c.r,c.g,c.b}); }
template<class T> Json encode(const T& value) {
    if constexpr(std::is_enum_v<T>) return static_cast<int>(value);
    else if constexpr(std::is_same_v<T,StylizationColor>) return encodeColor(value);
    else if constexpr(std::is_same_v<T,AnalogDisplaySettings>) {
        Json result=Json::object();
        detail::visitAnalogDisplayFields(value,[&](const char* key,const auto& field){result[key]=encode(field);});
        return result;
    }
    else if constexpr(std::is_same_v<T,TemporalFeedbackSettings>) {
        Json result=Json::object();
        detail::visitTemporalFeedbackFields(value,[&](const char* key,const auto& field){result[key]=encode(field);});
        return result;
    }
    else if constexpr(std::is_same_v<T,std::array<StylizationColor,16>>) {
        auto array=Json::array(); for(auto c:value) array.push_back(encodeColor(c)); return array;
    } else return value;
}
Json document(const ColorStylizationSettings& s,int version=3) {
    Json result{{"type","Aliyat.ColorStylizationLook"},{"schemaVersion",version}};
    const auto field=[&](const char* key,const auto& value){result[key]=encode(value);};
    if(version==1)detail::visitColorStylizationFieldsV1(s,field);
    else if(version==2)detail::visitColorStylizationFieldsV2(s,field);
    else detail::visitColorStylizationFields(s,field);
    return result;
}
template<class T> void decode(const Json& source,T& value) {
    if constexpr(std::is_same_v<T,bool>) {
        if(!source.is_boolean()) throw std::runtime_error("Expected boolean");
        value=source.get<bool>();
    } else if constexpr(std::is_enum_v<T> || std::is_integral_v<T>) {
        if(!source.is_number_integer()) throw std::runtime_error("Expected integer");
        const auto n=source.get<int64_t>();
        const int64_t upper=std::is_enum_v<T> ? 255 : static_cast<int64_t>(std::numeric_limits<T>::max());
        if(n<0 || n>upper) throw std::runtime_error("Integer out of range");
        value=static_cast<T>(n);
    } else if constexpr(std::is_floating_point_v<T>) {
        if(!source.is_number()) throw std::runtime_error("Expected finite number");
        value=source.get<T>(); if(!std::isfinite(value)) throw std::runtime_error("Expected finite number");
    } else if constexpr(std::is_same_v<T,StylizationColor>) {
        if(!source.is_array() || source.size()!=3) throw std::runtime_error("Expected three RGB components");
        decode(source[0],value.r);decode(source[1],value.g);decode(source[2],value.b);
    } else if constexpr(std::is_same_v<T,AnalogDisplaySettings>) {
        if(!source.is_object())throw std::runtime_error("Expected display object");
        detail::visitAnalogDisplayFields(value,[&](const char* key,auto& field){decode(source.at(key),field);});
    } else if constexpr(std::is_same_v<T,TemporalFeedbackSettings>) {
        if(!source.is_object())throw std::runtime_error("Expected temporal object");
        detail::visitTemporalFeedbackFields(value,[&](const char* key,auto& field){decode(source.at(key),field);});
    } else {
        if(!source.is_array() || source.size()!=value.size()) throw std::runtime_error("Expected 16 palette entries");
        for(size_t i=0;i<value.size();++i) decode(source[i],value[i]);
    }
}
bool fail(std::string* error,const std::string& message) {if(error)*error=message;return false;}
}

std::string serializeColorStylizationLook(const ColorStylizationSettings& settings) {
    return document(sanitizeColorStylization(settings)).dump(2)+"\n";
}
bool parseColorStylizationLook(std::string_view text,ColorStylizationSettings& output,std::string* error) {
    if(error)error->clear();
    if(text.empty() || text.size()>kMaxBytes) return fail(error,"Look document must contain 1..65536 bytes");
    try {
        std::array<std::set<std::string>,10> keys;
        auto callback=[&](int depth,Json::parse_event_t event,Json& parsed) {
            if(depth<0 || depth>=int(keys.size())) throw std::runtime_error("Look document nesting exceeds limit");
            if(event==Json::parse_event_t::object_start) keys[depth].clear();
            if(event==Json::parse_event_t::key && !keys[depth-1].insert(parsed.get<std::string>()).second)
                throw std::runtime_error("Duplicate look field");
            return true;
        };
        const auto root=Json::parse(text.begin(),text.end(),callback);
        if(!root.is_object() || !root.contains("type") || root["type"]!="Aliyat.ColorStylizationLook"
            || !root.contains("schemaVersion") || !root["schemaVersion"].is_number_integer()
            || (root["schemaVersion"]!=1 && root["schemaVersion"]!=2 && root["schemaVersion"]!=3)) return fail(error,"Unsupported look type or schema version");
        const int version=root["schemaVersion"].get<int>();
        ColorStylizationSettings settings;
        const auto field=[&](const char* key,auto& value){decode(root.at(key),value);};
        if(version==1)detail::visitColorStylizationFieldsV1(settings,field);
        else if(version==2)detail::visitColorStylizationFieldsV2(settings,field);
        else detail::visitColorStylizationFields(settings,field);
        settings=sanitizeColorStylization(settings);
        if(document(settings,version)!=root) return fail(error,"Unknown field or value outside the look contract");
        output=settings;return true;
    } catch(const std::exception& e) {return fail(error,e.what());}
}
bool loadColorStylizationLook(const std::string& path,ColorStylizationSettings& output,std::string* error) {
    if(error)error->clear();
    try {
        std::ifstream input(std::filesystem::u8path(path),std::ios::binary);
        if(!input) return fail(error,"Cannot open look document");
        std::string text(kMaxBytes+1,'\0');input.read(text.data(),static_cast<std::streamsize>(text.size()));
        if(input.bad()) return fail(error,"Cannot read look document");
        text.resize(static_cast<size_t>(input.gcount()));
        return parseColorStylizationLook(text,output,error);
    } catch(const std::exception& e) {return fail(error,e.what());}
}
bool saveColorStylizationLook(const std::string& path,const ColorStylizationSettings& settings,std::string* error) {
    if(error)error->clear();
    try {
        auto extension=std::filesystem::u8path(path).extension().string();
        for(auto& c:extension)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(extension!=".rsl")return fail(error,"Look documents must use the .rsl suffix");
        const auto text=serializeColorStylizationLook(settings);
        if(!io::File::atomicWriteUtf8(path,text.data(),text.size()))return fail(error,"Cannot atomically save look document");
        return true;
    } catch(const std::exception& e) {return fail(error,e.what());}
}
} // namespace ayt::render
