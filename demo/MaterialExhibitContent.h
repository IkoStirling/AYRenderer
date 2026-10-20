#pragma once
// Demo content data shared by the native viewer and the Editor sample builder.
// This is not a new engine resource format or gameplay API.
#include <AYMath/MathTypes.h>

#include <array>
#include <string>
#include <vector>
namespace ayt::render::demo
{
inline constexpr std::array<const char *, 17> kMaterialIds = {
    "metal",    "stone",    "wood",    "ceramic", "rubber", "paint",  "fabric",  "panel",  "copper",
    "concrete", "terrazzo", "leather", "asphalt", "brick",  "marble", "plastic", "acrylic"};
inline constexpr int kMaterialExhibitColumns = 8;
inline constexpr int kOpaqueMaterialCount = 16;
inline constexpr std::array<const char *, 9> kMaterialSceneNames = {
    "gallery", "facade",     "workbench",    "study",       "cutout",
    "shapes",  "collection", "shapes-extra", "transparency"};
inline constexpr std::array<const char *, 4> kMaterialVariantSuffixes = {"", "-clean", "-wet", "-worn"};
struct MaterialSceneRecipe
{
    std::string id;
    int scene, hero, variant;
};
inline std::vector<MaterialSceneRecipe> materialSceneRecipes()
{
    std::vector<MaterialSceneRecipe> recipes;
    for (int variant = 0; variant < int(kMaterialVariantSuffixes.size()); ++variant)
    {
        for (int scene = 0; scene < int(kMaterialSceneNames.size()); ++scene)
            recipes.push_back({std::string(kMaterialSceneNames[scene]) + kMaterialVariantSuffixes[variant],
                               scene, -1, variant});
        for (int hero = 0; hero < int(kMaterialIds.size()); ++hero)
            recipes.push_back({std::string("hero-") + kMaterialIds[hero] + kMaterialVariantSuffixes[variant],
                               0, hero, variant});
    }
    return recipes;
}
inline constexpr std::array<const char *, 3> kMaterialCutoutIds = {"screen-base-alpha", "screen-opacity-red",
                                                                   "screen-opacity-alpha"};
inline constexpr std::array<const char *, 3> kMaterialCutoutLabels = {"Base-color alpha", "Opacity red",
                                                                      "Opacity alpha"};
struct MaterialStudy
{
    const char *id;
    const char *label;
    int material;
};
// Each row changes one control, keeping its geometry, source maps and lights fixed.
inline constexpr std::array<MaterialStudy, 12> kMaterialStudies = {{
    {"study-uv-half", "UV 0.5x", 2},
    {"study-uv-one", "UV 1x", 2},
    {"study-uv-two", "UV 2x", 2},
    {"study-uv-four", "UV 4x", 2},
    {"study-normal-zero", "Normal 0", 3},
    {"study-normal-half", "Normal 0.5", 3},
    {"study-normal-one", "Normal 1", 3},
    {"study-normal-two", "Normal 2", 3},
    {"study-rough-low", "Rough -0.25", 0},
    {"study-rough-zero", "Rough +0", 0},
    {"study-rough-mid", "Rough +0.25", 0},
    {"study-rough-high", "Rough +0.5", 0},
}};
enum class MaterialMesh
{
    Cube,
    Sphere,
    Cylinder,
    Torus
};
struct MaterialObject
{
    MaterialMesh mesh;
    int material;
    math::FVector3 position, scale;
    float angle;
    int study = -1;
    int cutout = -1;
};
struct MaterialExhibit
{
    std::vector<MaterialObject> objects;
    math::FVector3 cameraPosition, cameraTarget;
    float fov = 48;
};
inline MaterialExhibit makeMaterialExhibit(int sceneIndex, int hero = -1)
{
    MaterialExhibit exhibit;
    constexpr auto cube = MaterialMesh::Cube, sphere = MaterialMesh::Sphere,
                   cylinder = MaterialMesh::Cylinder, torus = MaterialMesh::Torus;
    const auto add = [&](MaterialMesh mesh, int material, float x, float y, float z, float sx, float sy,
                         float sz, float angle = 0.f) {
        exhibit.objects.push_back({mesh, material, {x, y, z}, {sx, sy, sz}, angle});
    };
    if (sceneIndex == 8)
    {
        exhibit.cameraPosition = {7, 5, -10};
        exhibit.cameraTarget = {0, 1.4f, 1};
        exhibit.fov = 43;
        add(cube, 1, 0, -.2f, 1, 12, .25f, 8);
        add(cube, 13, 0, 1.8f, 3, 10.5f, 3.6f, .2f);
        const int opaque[] = {2, 3, 15};
        for (int i = 0; i < 3; ++i)
        {
            const float x = (i - 1) * 3.2f;
            add(sphere, opaque[i], x, 1.6f, 1.7f, 1.9f, 1.9f, 1.9f);
            add(cube, 16, x, 1.7f, 0, 2.8f, 3.1f, .025f);
            add(cylinder, 0, x - 1.45f, 1.55f, .02f, .09f, 3.1f, .09f);
            add(cylinder, 0, x + 1.45f, 1.55f, .02f, .09f, 3.1f, .09f);
            add(cube, 0, x, 3.15f, .02f, 2.95f, .09f, .09f);
        }
        add(cube, 16, 0, 1.2f, -2.2f, 1.6f, 2.2f, 1.3f, -.35f);
    }
    else if (sceneIndex == 6)
    {
        exhibit.cameraPosition = {10, 12, -15};
        exhibit.cameraTarget = {0, 1, 4.7f};
        exhibit.fov = 46;
        add(cube, 4, 0, -.25f, 4.7f, 12, .3f, 15);
        for (int i = 0; i < kOpaqueMaterialCount; ++i)
        {
            const float x = (i % 4 - 1.5f) * 2.5f, z = (i / 4) * 3.3f;
            add(cube, 1, x, .15f, z, 2, .5f, 2);
            add(sphere, i, x, 1.5f, z, 1.85f, 1.85f, 1.85f, .35f);
            add(cube, i, x, .49f, z - 1.07f, 1.7f, .12f, .42f);
        }
    }
    else if (sceneIndex == 5 || sceneIndex == 7)
    {
        exhibit.cameraPosition = {5, 12, -14};
        exhibit.cameraTarget = {0, .8f, 4.5f};
        exhibit.fov = 43;
        add(cube, 4, 0, -.28f, 4.5f, 24, .3f, 14);
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < kMaterialExhibitColumns; ++column)
            {
                const int material = column + (sceneIndex == 7 ? kMaterialExhibitColumns : 0);
                const auto mesh = row < 2 ? sphere : row == 2 ? cylinder : torus;
                add(mesh, material, (column - 3.5f) * 2.7f, row == 3 ? .8f : 1.25f, row * 3.f,
                    row == 1 ? 2.2f : 1.7f, row == 1 ? .8f : 1.7f, row == 1 ? 1.2f : 1.7f, .35f);
            }
    }
    else if (sceneIndex == 4)
    {
        exhibit.cameraPosition = {6, 4.5f, -10};
        exhibit.cameraTarget = {0, 1.5f, 0};
        exhibit.fov = 43;
        add(cube, 4, 0, -.2f, 0, 11, .25f, 6);
        for (int i = 0; i < 3; ++i)
        {
            const float x = (i - 1) * 3.f;
            add(cube, 0, x, 1.65f, 0, 2.4f, 3.f, .045f);
            exhibit.objects.back().cutout = i;
            add(cube, 5, x, .14f, 0, 2.6f, .3f, .7f);
            add(cylinder, 0, x - 1.3f, 1.6f, .12f, .1f, 3.2f, .1f);
            add(cylinder, 0, x + 1.3f, 1.6f, .12f, .1f, 3.2f, .1f);
        }
        add(sphere, 3, 0, 1.6f, 1.8f, 2.2f, 2.2f, 2.2f);
    }
    else if (sceneIndex == 3)
    {
        exhibit.cameraPosition = {8.5f, 10, -14};
        exhibit.cameraTarget = {0, .8f, 1.5f};
        exhibit.fov = 44;
        add(cube, 4, 0, -.25f, 1.7f, 12, .3f, 10);
        for (size_t i = 0; i < kMaterialStudies.size(); ++i)
        {
            const float x = (int(i % 4) - 1.5f) * 2.6f, z = int(i / 4) * 3.2f;
            add(cube, 1, x, .15f, z, 2.15f, .5f, 2.15f);
            add(i < 4 ? cube : sphere, kMaterialStudies[i].material, x, 1.45f, z, 1.8f, 1.8f, 1.8f, .3f);
            exhibit.objects.back().study = int(i);
        }
    }
    else if (hero >= 0)
    {
        exhibit.cameraPosition = {5, 3.2f, -6.5f};
        exhibit.cameraTarget = {0, 1, 0};
        exhibit.fov = 43;
        add(cube, 4, 0, -.18f, 0, 8, .25f, 6);
        add(sphere, hero, -1.4f, 1.5f, 0, 2.4f, 2.4f, 2.4f, .3f);
        add(cube, hero, 1.6f, 1.1f, 0, 1.8f, 2.2f, 1.5f, -.35f);
        add(cube, hero, 0, .05f, -1.8f, 4, .1f, .8f);
    }
    else if (sceneIndex == 0)
    {
        exhibit.cameraPosition = {8, 7, -11};
        exhibit.cameraTarget = {0, 1, 1};
        exhibit.fov = 48;
        add(cube, 4, 0, -.25f, 1, 12, .3f, 8);
        for (int i = 0; i < 8; ++i)
        {
            const float x = (i % 4 - 1.5f) * 2.5f, z = (i / 4) * 3.3f;
            add(cube, 1, x, .15f, z, 2, .5f, 2);
            add(sphere, i, x, 1.5f, z, 1.85f, 1.85f, 1.85f, .35f);
            add(cube, i, x, .49f, z - 1.07f, 1.7f, .12f, .42f);
        }
    }
    else if (sceneIndex == 1)
    {
        exhibit.cameraPosition = {9, 6, -11};
        exhibit.cameraTarget = {0, 2, 1};
        exhibit.fov = 47;
        add(cube, 1, 0, -.2f, 1, 13, .25f, 8);
        add(cube, 1, 0, 2.2f, 1, 8, 4.4f, 2.6f);
        for (int floor = 0; floor < 2; ++floor)
            for (int bay = 0; bay < 4; ++bay)
            {
                const float x = (bay - 1.5f) * 1.8f, y = 1.3f + floor * 1.9f;
                add(cube, 2, x, y, -.43f, 1.35f, 1.6f, .18f);
                add(cube, 3, x, y, -.56f, 1.05f, 1.32f, .12f);
                add(cube, 0, x, y - .78f, -.75f, 1.5f, .13f, .5f);
            }
        add(cube, 5, 0, 4.5f, -.4f, 8.6f, .5f, .8f);
        add(cube, 7, 0, 4.5f, -.83f, 4, .23f, .06f);
        for (int i = 0; i < 3; ++i)
        {
            add(cylinder, 0, -4.7f + i * .35f, 1.75f, 1, .2f, 3.5f, .2f);
            add(torus, 4, -4.7f + i * .35f, 2.7f, 1, .3f, .3f, .3f);
        }
        add(cube, 2, 3.7f, .18f, -2.2f, 2.5f, .3f, 1.2f);
        add(cube, 6, 3.7f, .45f, -2.2f, 2.2f, .25f, .9f);
    }
    else
    {
        exhibit.cameraPosition = {7.5f, 6, -9};
        exhibit.cameraTarget = {0, 1.2f, 0};
        exhibit.fov = 43;
        add(cube, 1, 0, -.2f, 0, 11, .25f, 8);
        add(cube, 2, 0, 1, 0, 7, .4f, 4);
        for (float x : {-2.7f, 2.7f})
            for (float z : {-1.3f, 1.3f})
                add(cube, 0, x, .4f, z, .3f, 1.2f, .3f);
        add(cube, 4, 0, 1.24f, -.35f, 4.5f, .08f, 2.3f);
        add(cylinder, 0, -1.3f, 1.8f, 0, 1.6f, 1.1f, 1.6f);
        add(torus, 4, -1.3f, 2.36f, 0, 1.4f, 1.4f, 1.4f);
        add(cylinder, 5, 1.25f, 1.7f, .4f, 1.2f, .8f, 1.2f);
        add(sphere, 0, 1.25f, 2.18f, .4f, .9f, .55f, .9f);
        add(cube, 7, .5f, 1.35f, -1.35f, 2, .18f, .65f, -.15f);
        add(cube, 6, -2.8f, 1.3f, -.7f, .9f, .15f, 1.5f, .2f);
        for (int i = 0; i < 5; ++i)
            add(cylinder, 0, -1.9f + i * .55f, 1.38f, 1.45f, .16f, .3f, .16f);
    }
    return exhibit;
}
} // namespace ayt::render::demo
