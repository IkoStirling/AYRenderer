#include "AYRenderer/PbrShaderSources.h"
#include "AYRenderer/PbrSurfaceSettings.h"
#include "AYResource/assetsImpl/Material.h"
#include <AYIO/File.h>
#include <AYIO/PathSafety.h>
#include <AYStorage/Guid.h>
#include <nlohmann/json.hpp>
#include "DemoUtf8Arguments.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;
std::string utf8(const fs::path &path)
{
    const auto s = path.u8string();
    return {s.begin(), s.end()};
}
float number(const Json &value, float low, float high)
{
    if (!value.is_number())
        throw std::runtime_error("Expected number");
    const auto v = value.get<float>();
    if (!std::isfinite(v) || v < low || v > high)
        throw std::runtime_error("Material value outside its range");
    return v;
}
template <size_t N> std::array<float, N> numbers(const Json &value, float low, float high)
{
    if (!value.is_array() || value.size() != N)
        throw std::runtime_error("Incorrect vector length");
    std::array<float, N> result;
    for (size_t i = 0; i < N; ++i)
        result[i] = number(value[i], low, high);
    return result;
}
struct Output
{
    fs::path path;
    std::vector<UInt8> bytes;
};
int build(const fs::path &root)
{
    std::ifstream input(root / "library.json", std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 1024 * 1024)
        throw std::runtime_error("library.json must contain 1..1048576 bytes");
    const auto count = size_t(input.tellg());
    std::string text(count, '\0');
    input.seekg(0);
    input.read(text.data(), count);
    if (!input)
        throw std::runtime_error("Could not read library.json");
    const auto recipe = Json::parse(text);
    if (!recipe.is_object() || recipe.size() != 2 || !recipe.at("schemaVersion").is_number_integer() ||
        recipe.at("schemaVersion") != 1 || !recipe.at("materials").is_array() ||
        recipe.at("materials").empty() || recipe.at("materials").size() > 128)
        throw std::runtime_error("Unsupported demo material recipe");
    std::set<std::string> names;
    std::vector<Output> outputs;
    const std::set<std::string> fields = {
        "id",       "name",      "baseColor",   "metallic",       "roughness",
        "ao",       "emissive",  "uvTransform", "normalStrength", "roughnessBias",
        "textures", "alphaMode", "alphaCutoff", "doubleSided",    "opacitySource"};
    const std::set<std::string> slots = {"baseColorTexture", "normalTexture", "metallicTexture",
                                         "roughnessTexture", "aoTexture",     "emissiveTexture",
                                         "opacityTexture"};
    for (const auto &item : recipe.at("materials"))
    {
        if (!item.is_object())
            throw std::runtime_error("Material recipe must be an object");
        for (auto it = item.begin(); it != item.end(); ++it)
            if (!fields.contains(it.key()))
                throw std::runtime_error("Unknown material field: " + it.key());
        const auto id = item.at("id").get<std::string>();
        if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z' ||
            !std::all_of(
                id.begin(), id.end(),
                [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }) ||
            !names.insert(id).second)
            throw std::runtime_error("Invalid or duplicate material id");
        ayt::resource::Material material;
        const auto title = item.at("name").get<std::string>();
        if (title.empty() || title.size() > 256 || title.find('\0') != std::string::npos)
            throw std::runtime_error("Material name must contain 1..256 UTF-8 bytes without NUL");
        material.initialise(title, "pbr.phoskia");
        for (const char *enumName : {"alphaMode", "opacitySource"})
            if (item.contains(enumName) && !item.at(enumName).is_number_integer())
                throw std::runtime_error(std::string(enumName) + " must be an integer");
        const int alpha = item.value("alphaMode", 0);
        if (alpha < 0 || alpha > 2)
            throw std::runtime_error("Unsupported alpha mode");
        material.setSurfaceProperties(static_cast<ayt::resource::MaterialAlphaMode>(alpha),
                                      number(item.value("alphaCutoff", Json(.5)), 0, 1),
                                      item.value("doubleSided", false));
        const auto color = numbers<4>(item.at("baseColor"), 0, 1);
        material.setFloat4("baseColor", color.data());
        const auto emission = numbers<3>(item.at("emissive"), 0, 1);
        material.setFloat3("emissive", emission.data());
        for (const char *name : {"metallic", "roughness", "ao"})
            material.setFloat(name, number(item.at(name), 0, 1));
        auto uv = numbers<4>(item.at("uvTransform"), -1024, 1024);
        number(item.at("uvTransform")[0], .01f, 128);
        number(item.at("uvTransform")[1], .01f, 128);
        material.setFloat4("uvTransform", uv.data());
        material.setFloat("normalStrength", number(item.at("normalStrength"), 0, 4));
        material.setFloat("roughnessBias", number(item.at("roughnessBias"), -1, 1));
        material.setFloat("normalYSign", 1);
        material.setFloat("opacity", 1);
        const auto opacitySource = item.value("opacitySource", 0);
        if (opacitySource < 0 || opacitySource > 2)
            throw std::runtime_error("Unsupported opacity source");
        material.setFloat("opacitySource", float(opacitySource));
        material.setFloat("premultipliedAlpha", 0);
        material.setFloat("castSkinned", 0);
        if (!item.at("textures").is_object())
            throw std::runtime_error("Expected texture map");
        for (auto it = item.at("textures").begin(); it != item.at("textures").end(); ++it)
        {
            if (!slots.contains(it.key()))
                throw std::runtime_error("Unknown texture slot: " + it.key());
            const auto ref = it.value().get<std::string>();
            fs::path resolved;
            std::string reason;
            if (fs::u8path(ref).is_absolute() ||
                !ayt::io::path::resolveWithinRoot(root, root / fs::u8path(ref), resolved, reason) ||
                !fs::is_regular_file(resolved))
                throw std::runtime_error("Missing or escaped texture: " + ref);
            // textures/ is a reserved cooked virtual-root prefix. This demo
            // authors a portable loose directory, so mark references explicitly
            // owner-relative instead of depending on the process asset root.
            const auto ownerRelative = "./" + ref;
            material.setTexture(it.key().c_str(), ownerRelative.c_str());
        }
        // The existing resource encoder owns the binary schema. This identity
        // covers this material's authored recipe; texture contents have their
        // own resource/cache identities and are not embedded in the material.
        const auto identity = item.dump();
        material.setGuid(ayt::storage::Guid::computeFromData(identity.data(), identity.size()));
        Output output;
        output.path = root / (id + ".mat");
        if (!material.saveToBinary(output.bytes))
            throw std::runtime_error("Material encoder failed");
        ayt::resource::Material check;
        if (!check.loadFromBinary(output.bytes.data(), output.bytes.size()))
            throw std::runtime_error("Encoded material did not load");
        outputs.push_back(std::move(output));
    }
    std::string shader = ayt::render::kPbrPhoskiaSource;
    if (!shader.empty() && shader.front() == '\n')
        shader.erase(shader.begin());
    if (!ayt::io::File::atomicWriteUtf8(utf8(root / "pbr.phoskia"), shader.data(), shader.size()))
        throw std::runtime_error("Could not save PBR source");
    for (const auto &output : outputs)
    {
        if (!ayt::io::File::atomicWriteUtf8(utf8(output.path), output.bytes.data(), output.bytes.size()))
            throw std::runtime_error("Could not save " + utf8(output.path));
        std::printf("Saved %s (%zu bytes)\n", utf8(output.path).c_str(), output.bytes.size());
    }
    return 0;
}
int run(int argc, char **argv)
{
    try
    {
        if (argc == 3 && std::string_view(argv[1]) == "build")
            return build(fs::weakly_canonical(fs::u8path(argv[2])));
        if (argc == 3 && std::string_view(argv[1]) == "inspect")
        {
            std::ifstream input(fs::u8path(argv[2]), std::ios::binary | std::ios::ate);
            if (!input || input.tellg() <= 0 || input.tellg() > 1024 * 1024)
                throw std::runtime_error("Invalid material file size");
            std::vector<uint8_t> bytes(size_t(input.tellg()));
            input.seekg(0);
            input.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
            ayt::resource::Material material;
            if (!input || !material.loadFromBinary(bytes.data(), bytes.size()))
                throw std::runtime_error("Could not load material");
            std::printf("%s | shader=%s | parameters=%u\n", material.getName(), material.getShader(),
                        material.getParameterCount());
            return 0;
        }
        std::fprintf(stderr,
                     "Usage: AYRenderer_MaterialTool build <library-directory> | inspect <material.mat>\n");
        return 2;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    ayt::render::demo::Utf8Arguments arguments(argc, argv);
    return run(argc, arguments.pointers.data());
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif
