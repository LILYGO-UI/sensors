#pragma once

#include "pages/sensors/sensors_view_model.hpp"
#include "pages/keyboard/keyboard_view.hpp"

#include <vector>

namespace sensors {

class SensorsView {
public:
    explicit SensorsView(SensorsViewModel &view_model,
                         std::unique_ptr<KeyboardSource> keyboard_source = make_keyboard_source())
        : view_model_(view_model), keyboard_(std::move(keyboard_source)) {}
    ~SensorsView() { destroy(); }
    lv_obj_t *create(lv_obj_t *parent, const char *title);
    void destroy();

private:
    struct Row {
        std::string id;
        lv_obj_t *object = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *location = nullptr;
        lv_obj_t *status = nullptr;
    };
    struct Channel {
        lv_obj_t *value = nullptr;
        lv_obj_t *name = nullptr;
        lv_obj_t *extrema = nullptr;
        lv_obj_t *record = nullptr;
    };
    struct Measurement {
        std::string id;
        lv_obj_t *object = nullptr;
        lv_obj_t *plot = nullptr;
        lv_obj_t *error = nullptr;
        lv_obj_t *chart = nullptr;
        lv_obj_t *range = nullptr;
        lv_obj_t *times = nullptr;
        std::vector<Channel> channels;
        std::vector<lv_chart_series_t *> series;
    };
    static void changed(lv_observer_t *observer, lv_subject_t *subject);
    static void tick(lv_timer_t *timer);
    static void command(lv_event_t *event);
    static void tooltip_event(lv_event_t *event);
    static void layout_event(lv_event_t *event);
    void layout();
    void refresh();
    void refresh_rows();
    void refresh_detail();
    void refresh_chart(Measurement &view, const SensorReading &sensor);
    void refresh_buses();
    void show_tab(unsigned tab);
    void show_page();
    void refresh_keyboard_header();

    SensorsViewModel &view_model_;
    KeyboardView keyboard_;
    lv_obj_t *surface_ = nullptr, *header_ = nullptr, *status_ = nullptr;
    lv_obj_t *back_ = nullptr, *title_ = nullptr, *list_recording_ = nullptr;
    lv_obj_t *pause_ = nullptr, *pause_icon_ = nullptr, *clear_ = nullptr, *rescan_ = nullptr;
    lv_obj_t *record_ = nullptr, *record_icon_ = nullptr, *tooltip_ = nullptr;
    lv_obj_t *navigation_ = nullptr, *tabs_ = nullptr, *filter_ = nullptr, *interval_ = nullptr;
    lv_obj_t *count_ = nullptr, *connection_error_ = nullptr, *body_ = nullptr;
    lv_obj_t *list_panel_ = nullptr, *list_ = nullptr, *list_empty_ = nullptr;
    lv_obj_t *detail_ = nullptr, *detail_name_ = nullptr, *detail_category_ = nullptr;
    lv_obj_t *detail_status_ = nullptr, *detail_error_ = nullptr, *detail_empty_ = nullptr;
    lv_obj_t *readings_ = nullptr, *plot_header_ = nullptr, *plots_ = nullptr;
    lv_obj_t *metadata_ = nullptr, *recording_ = nullptr;
    lv_obj_t *buses_ = nullptr, *keyboard_page_ = nullptr, *keyboard_entry_ = nullptr, *sampling_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    std::vector<Row> rows_;
    std::vector<Measurement> measurements_;
    std::vector<int> filters_;
    std::string row_signature_, bus_signature_, channel_signature_;
    std::string app_title_;
    unsigned tab_ = 0;
    bool laying_out_ = false, detail_visible_ = false, keyboard_visible_ = false;
};

}  // namespace sensors
