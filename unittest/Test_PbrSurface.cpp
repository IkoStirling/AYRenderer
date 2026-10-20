#include <AYTest.h>
#include "AYRenderer.h"
#include "detail/PbrSurfaceBinding.h"
#include "detail/RenderResourceManager.h"
#include "AYResource/assetsImpl/Material.h"
#include <limits>
#include <cmath>

TEST_SUITE(AYRenderer_PbrSurface)
TEST_CASE(surface_controls_sanitize_identity_and_finite_limits)
{
    using namespace ayt::render;
    const auto defaults = sanitizePbrSurface({});
    CHECK(defaults.uvScaleX == 1);
    CHECK(defaults.uvScaleY == 1);
    CHECK(defaults.uvOffsetX == 0);
    CHECK(defaults.uvOffsetY == 0);
    CHECK(defaults.normalStrength == 1);
    CHECK(defaults.roughnessBias == 0);
    PbrSurfaceSettings value{-8, 500, -2000, 2000, -4, 8};
    value = sanitizePbrSurface(value);
    CHECK(value.uvScaleX == .01f);
    CHECK(value.uvScaleY == 128);
    CHECK(value.uvOffsetX == -1024);
    CHECK(value.uvOffsetY == 1024);
    CHECK(value.normalStrength == 0);
    CHECK(value.roughnessBias == 1);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    value = sanitizePbrSurface({nan, inf, nan, inf, nan, inf});
    CHECK(value.uvScaleX == 1);
    CHECK(value.uvScaleY == 1);
    CHECK(value.uvOffsetX == 0);
    CHECK(value.uvOffsetY == 0);
    CHECK(value.normalStrength == 1);
    CHECK(value.roughnessBias == 0);
}
TEST_CASE(material_semantics_survive_specialized_programs_and_do_not_bleed_to_legacy)
{
    using namespace ayt::render;
    using namespace ayt::render::detail;
    BGFXAdapter adapter;
    ayt::shader::ShaderResourcePool pool;
    RenderResourceManager resources(adapter, pool);
    resources.materials().try_emplace(41);
    resources.materials().try_emplace(42);
    resources.setMaterialPbrSurfaceSettings({41}, {3, 5, .25f, -.5f, 0, -.3f});
    const auto custom = resources.materialPbrSurfaceSettings({41});
    CHECK(custom.uvScaleX == 3);
    CHECK(custom.uvScaleY == 5);
    CHECK(custom.uvOffsetX == .25f);
    CHECK(custom.uvOffsetY == -.5f);
    CHECK(custom.normalStrength == 0);
    CHECK(custom.roughnessBias == -.3f);
    const auto legacy = resources.materialPbrSurfaceSettings({42});
    CHECK(legacy.uvScaleX == 1);
    CHECK(legacy.uvScaleY == 1);
    CHECK(legacy.uvOffsetX == 0);
    CHECK(legacy.normalStrength == 1);
    CHECK(legacy.roughnessBias == 0);
    resources.setMaterialFloat({41}, "normalStrength", 8);
    CHECK(resources.materialPbrSurfaceSettings({41}).normalStrength == 4);
    resources.setMaterialPbrSurfaceSettings({999}, {3, 3, 0, 0, 4, 1});
    CHECK(resources.materials().size() == 2);
    CHECK(resources.materialPbrSurfaceSettings({999}).uvScaleX == 1);
    CHECK(resources.materials().at(41).uniformSlots.size() == 3);
    Renderer renderer;
    renderer.setMaterialPbrSurfaceSettings({}, custom);
    CHECK(renderer.materialPbrSurfaceSettings({}).normalStrength == 1);
}
TEST_CASE(existing_material_binary_round_trips_surface_parameters_without_new_abi)
{
    using namespace ayt::resource;
    Material original;
    original.initialise("Surface", "pbr.phoskia");
    const float uv[4] = {3, 2, .125f, .5f};
    original.setFloat4("uvTransform", uv);
    original.setFloat("normalStrength", .4f);
    original.setFloat("roughnessBias", -.15f);
    original.setTexture("normalTexture", "textures/stone_n.png");
    std::vector<UInt8> bytes;
    CHECK(original.saveToBinary(bytes));
    Material restored;
    CHECK(restored.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(std::string(restored.getName()) == "Surface");
    CHECK(restored.getParameterType("uvTransform") == MaterialParamType::Float4);
    CHECK(restored.getVector4("uvTransform").x == 3);
    CHECK(restored.getVector4("uvTransform").w == .5f);
    CHECK(restored.getFloat("normalStrength") == .4f);
    CHECK(restored.getFloat("roughnessBias") == -.15f);
    CHECK(std::string(restored.getTexture("normalTexture")) == "textures/stone_n.png");
    CHECK(IMaterial::ABI_VERSION == 3);
}
TEST_SUITE_END
