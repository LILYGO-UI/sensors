#include "app.hpp"

#include "app_identity.h"
#include "pages/sensors/sensors_view.hpp"
#include "pages/sensors/sensors_view_model.hpp"

#include <memory>

namespace {

struct AppState {
    std::unique_ptr<sensors::SensorsViewModel> view_model;
    std::unique_ptr<sensors::SensorsView> view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    app_state.view_model = std::make_unique<sensors::SensorsViewModel>();
    app_state.view = std::make_unique<sensors::SensorsView>(*app_state.view_model);
    app_state.view->create(context->root, LILYGO_UI_SENSORS_APP_NAME);
}

void app_close()
{
    if (app_state.view) app_state.view->destroy();
    app_state.view.reset();
    app_state.view_model.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t),
    LILYGO_UI_SENSORS_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
