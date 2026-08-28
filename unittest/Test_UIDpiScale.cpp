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

TEST_SUITE_END
