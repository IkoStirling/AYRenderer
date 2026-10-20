#pragma once
// Strict loose-content checks for this self-contained demo, before device work.
// Production ResourceManager fallback policy is unchanged.
#include <AYResource/AssetPath.h>
#include <AYResource/assetsImpl/Material.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ayt::render::demo
{
inline std::string materialPathUtf8(const std::filesystem::path &path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
inline bool readWorkshopMaterial(const std::filesystem::path &path, resource::Material &material,
                                 std::string &error)
{
    try
    {
        const auto owner = materialPathUtf8(path);
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() < 1 || input.tellg() > 4 * 1024 * 1024)
        {
            error = "Missing or oversized material: " + owner;
            return false;
        }
        std::vector<UInt8> bytes(size_t(input.tellg()));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) ||
            !material.loadFromBinary(bytes.data(), bytes.size()))
        {
            error = "Invalid single material: " + owner;
            return false;
        }
        const auto shader = resource::resolveAssetPath(owner, material.getShader());
        if (!std::filesystem::is_regular_file(std::filesystem::u8path(shader)))
        {
            error = "Missing shader for " + owner + ": " + shader;
            return false;
        }
        bool valid = true;
        material.forEachParameter([&](const char *name, resource::MaterialParamType type) {
            if (!valid || (type != resource::MaterialParamType::Texture2D &&
                           type != resource::MaterialParamType::Texture3D &&
                           type != resource::MaterialParamType::TextureCube))
                return;
            const auto *reference = material.getTexture(name);
            if (!reference || !*reference)
                return;
            const auto resolved = resource::resolveAssetPath(owner, reference);
            if (!std::filesystem::is_regular_file(std::filesystem::u8path(resolved)))
            {
                error = "Missing texture " + std::string(name) + " for " + owner + ": " + resolved;
                valid = false;
            }
        });
        if (valid)
            error.clear();
        return valid;
    }
    catch (const std::exception &ex)
    {
        error = std::string("Invalid material dependency: ") + ex.what();
        return false;
    }
}
} // namespace ayt::render::demo
