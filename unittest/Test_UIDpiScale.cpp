#include "AYTest.h"
#include "AYRenderer/UIRenderBackend.h"

#include <limits>

using namespace ayt::render;

TEST_SUITE(AYRenderer_UIDpiScale)

TEST_CASE(ui_backend_retains_scale_without_changing_object_layout) {
    UIRenderBackend backend;
    CHECK_FLOAT_EQ(backend.getUiScale(), 1.0f, 1e-6f);
    backend.setUiScale(2.0f);
    CHECK_FLOAT_EQ(backend.getUiScale(), 2.0f, 1e-6f);
    backend.beginFrame();
    CHECK_FLOAT_EQ(backend.getUiScale(), 2.0f, 1e-6f);
}

TEST_CASE(ui_backend_rejects_invalid_scale) {
    UIRenderBackend backend;
    backend.setUiScale(0.0f);
    CHECK_FLOAT_EQ(backend.getUiScale(), 1.0f, 1e-6f);
    backend.setUiScale(std::numeric_limits<float>::quiet_NaN());
    CHECK_FLOAT_EQ(backend.getUiScale(), 1.0f, 1e-6f);
}

TEST_CASE(ui_backend_scopes_uniform_viewport_transform_scale) {
    UIRenderBackend backend;
    backend.setUiScale(1.5f);
    const ayt::math::Float4x4 transform(
        2.0f, 0.0f, 0.0f, 40.0f,
        0.0f, 2.0f, 0.0f, 24.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    backend.pushTransform(transform);
    CHECK_FLOAT_EQ(backend.getUiScale(), 3.0f, 1e-6f);
    backend.pushTransform(transform);
    CHECK_FLOAT_EQ(backend.getUiScale(), 6.0f, 1e-6f);
    backend.popTransform();
    CHECK_FLOAT_EQ(backend.getUiScale(), 3.0f, 1e-6f);
    backend.popTransform();
    CHECK_FLOAT_EQ(backend.getUiScale(), 1.5f, 1e-6f);
}

TEST_CASE(ui_backend_recovers_unbalanced_viewport_transform_next_frame) {
    UIRenderBackend backend;
    backend.setUiScale(2.0f);
    const ayt::math::Float4x4 transform(
        1.25f, 0.0f, 0.0f, 10.0f,
        0.0f, 1.25f, 0.0f, 20.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    backend.pushTransform(transform);
    CHECK_FLOAT_EQ(backend.getUiScale(), 2.5f, 1e-6f);
    backend.beginFrame();
    CHECK_FLOAT_EQ(backend.getUiScale(), 2.0f, 1e-6f);
}

TEST_SUITE_END
