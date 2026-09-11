#include "pages/sensors/sensors_view.hpp"
#include "components/components.hpp"
#include <cm0/typography.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

namespace sensors {
namespace {
// State roles: teal live, red faults/X, green Y, cyan Z, amber demo/recording.
constexpr uint32_t live_color = 0x087f78, muted_color = 0x657078, fault_color = 0xc43d4e;
constexpr uint32_t amber_color = 0xa56808;
constexpr uint32_t channel_colors[] = {0xc43d4e, 0x238452, 0x008ca8, 0x9b5aab, 0x936b22};
constexpr const char *tab_labels[] = {"Sensors", "Buses", ""};

lv_obj_t *plain(lv_obj_t *parent)
{
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}

void background(lv_obj_t *object, uint32_t color)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
}

void font(lv_obj_t *object, int size, lv_style_selector_t selector = 0)
{
    if (auto *value = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(object, value, selector);
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int size = 18,
                uint32_t color = components::color_text)
{
    auto *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    font(object, size);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_WRAP);
    return object;
}

lv_obj_t *column(lv_obj_t *parent)
{
    auto *object = plain(parent);
    lv_obj_set_size(object, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_COLUMN);
    return object;
}

void hidden(lv_obj_t *object, bool value)
{
    if (value) lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
}

std::string number(double value)
{
    if (!std::isfinite(value)) return "--";
    char text[48];
    const double magnitude = std::abs(value);
    if (magnitude >= 100000 || (magnitude > 0 && magnitude < 0.001))
        std::snprintf(text, sizeof(text), "%.3g", value);
    else
        std::snprintf(text, sizeof(text), magnitude >= 1000 ? "%.1f" : "%.2f", value);
    return text;
}

void separator(lv_obj_t *object)
{
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_side(object, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(components::color_border), 0);
}
}  // namespace

lv_obj_t *SensorsView::create(lv_obj_t *parent, const char *title)
{
    destroy();
    app_title_ = title;
    surface_ = components::create_page(parent);
    lv_obj_set_style_pad_row(surface_, 0, 0);
    header_ = plain(surface_);
    lv_obj_set_name_static(header_, "app_header");
    lv_obj_set_size(header_, LV_PCT(100), 80);
    background(header_, components::color_container_background);
    lv_obj_set_style_pad_hor(header_, 12, 0);
    lv_obj_set_style_pad_column(header_, 6, 0);
    lv_obj_set_flex_flow(header_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    back_ = lv_button_create(header_);
    lv_obj_set_name_static(back_, "sensors_back");
    lv_obj_set_size(back_, 52, 52);
    lv_obj_set_style_radius(back_, 6, 0);
    lv_obj_set_style_shadow_width(back_, 0, 0);
    background(back_, components::color_page_background);
    lv_obj_center(label(back_, LV_SYMBOL_LEFT, 26, components::color_action));
    lv_obj_add_event_cb(back_, command, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(back_, tooltip_event, LV_EVENT_ALL, this);
    auto *identity = column(header_);
    lv_obj_set_width(identity, 0);
    lv_obj_set_flex_grow(identity, 1);
    title_ = label(identity, title, 26, components::color_container_title);
    lv_obj_set_name_static(title_, "app_title");
    lv_obj_set_width(title_, LV_PCT(100));
    lv_obj_set_height(title_, lv_obj_get_style_text_font(title_, LV_PART_MAIN)->line_height);
    lv_label_set_long_mode(title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    status_ = label(identity, "Disconnected", 18, muted_color);
    lv_obj_set_name_static(status_, "sensors_status");
    lv_obj_set_width(status_, LV_PCT(100));
    lv_obj_set_height(status_, lv_obj_get_style_text_font(status_, LV_PART_MAIN)->line_height);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    const char *icons[] = {LV_SYMBOL_PAUSE, LV_SYMBOL_TRASH, LV_SYMBOL_REFRESH, LV_SYMBOL_SAVE};
    const char *names[] = {"sensors_pause", "sensors_clear", "sensors_rescan", "sensors_record"};
    lv_obj_t **buttons[] = {&pause_, &clear_, &rescan_, &record_};
    for (int index = 0; index < 4; ++index) {
        auto *button = lv_button_create(header_);
        *buttons[index] = button;
        lv_obj_set_name_static(button, names[index]);
        lv_obj_set_size(button, 52, 52);
        lv_obj_set_style_radius(button, 6, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        background(button, components::color_page_background);
        auto *glyph = label(button, icons[index], 26, components::color_action);
        lv_obj_center(glyph);
        if (index == 0) pause_icon_ = glyph;
        if (index == 3) record_icon_ = glyph;
        lv_obj_add_event_cb(button, command, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(button, tooltip_event, LV_EVENT_ALL, this);
    }

    navigation_ = plain(surface_);
    lv_obj_set_name_static(navigation_, "sensors_navigation");
    lv_obj_set_size(navigation_, LV_PCT(100), 60);
    lv_obj_set_style_pad_hor(navigation_, 12, 0);
    lv_obj_set_style_pad_column(navigation_, 12, 0);
    background(navigation_, components::color_container_background);
    lv_obj_set_flex_flow(navigation_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navigation_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tabs_ = lv_buttonmatrix_create(navigation_);
    lv_obj_set_name_static(tabs_, "sensors_tabs");
    lv_obj_set_size(tabs_, 220, 48);
    lv_buttonmatrix_set_map(tabs_, tab_labels);
    lv_buttonmatrix_set_button_ctrl_all(tabs_, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(tabs_, true);
    lv_buttonmatrix_set_button_ctrl(tabs_, 0, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_style_pad_all(tabs_, 2, 0);
    lv_obj_set_style_pad_column(tabs_, 2, 0);
    lv_obj_set_style_border_width(tabs_, 0, 0);
    lv_obj_set_style_radius(tabs_, 6, 0);
    background(tabs_, components::color_page_background);
    font(tabs_, 18, LV_PART_ITEMS);
    lv_obj_set_style_radius(tabs_, 4, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(tabs_, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(tabs_, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(tabs_, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(tabs_, lv_color_hex(components::color_action), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(tabs_, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabs_, lv_color_hex(components::color_text), LV_PART_ITEMS);
    lv_obj_set_style_text_color(tabs_, lv_color_hex(components::color_container_background), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(tabs_, command, LV_EVENT_VALUE_CHANGED, this);
    filter_ = lv_dropdown_create(navigation_);
    lv_obj_set_name_static(filter_, "sensors_filter");
    lv_obj_set_size(filter_, 0, 48);
    lv_obj_set_flex_grow(filter_, 1);
    lv_obj_set_style_max_width(filter_, 220, 0);
    font(filter_, 18);
    font(lv_dropdown_get_list(filter_), 18);
    lv_dropdown_set_options(filter_, "All buses\nSystem");
    filters_ = {-1, -2};
    lv_obj_add_event_cb(filter_, command, LV_EVENT_VALUE_CHANGED, this);

    auto *sampling = plain(surface_);
    sampling_ = sampling;
    lv_obj_set_name_static(sampling, "sensors_sampling");
    lv_obj_set_size(sampling, LV_PCT(100), 60);
    lv_obj_set_style_pad_hor(sampling, 12, 0);
    lv_obj_set_style_pad_column(sampling, 8, 0);
    lv_obj_set_flex_flow(sampling, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sampling, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    separator(sampling);
    count_ = label(sampling, "0 sensors", 18, muted_color);
    lv_obj_set_name_static(count_, "sensors_count");
    lv_obj_set_width(count_, 0);
    lv_obj_set_flex_grow(count_, 1);
    interval_ = lv_dropdown_create(sampling);
    lv_obj_set_name_static(interval_, "sensors_interval");
    lv_obj_set_size(interval_, 140, 48);
    lv_dropdown_set_options(interval_, "100 ms\n250 ms\n1000 ms");
    font(interval_, 18);
    font(lv_dropdown_get_list(interval_), 18);
    lv_obj_add_event_cb(interval_, command, LV_EVENT_VALUE_CHANGED, this);
    for (auto *dropdown : {filter_, interval_}) {
        auto *options = lv_dropdown_get_list(dropdown);
        const int spacing = std::max(0, 48 - static_cast<int>(lv_obj_get_style_text_font(options, LV_PART_MAIN)->line_height));
        lv_obj_set_style_text_line_space(options, spacing, 0);
        lv_obj_set_style_text_line_space(options, spacing, LV_PART_SELECTED);
    }

    connection_error_ = label(surface_, "", 18, fault_color);
    lv_obj_set_name_static(connection_error_, "sensors_connection_error");
    lv_obj_set_width(connection_error_, LV_PCT(100));
    lv_obj_set_style_pad_hor(connection_error_, 12, 0);
    lv_obj_set_style_pad_ver(connection_error_, 8, 0);
    lv_obj_set_style_max_height(connection_error_, 72, 0);
    lv_label_set_long_mode(connection_error_, LV_LABEL_LONG_DOT);

    body_ = plain(surface_);
    lv_obj_set_name_static(body_, "sensors_body");
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_height(body_, 0);
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    lv_obj_set_style_pad_gap(body_, 1, 0);
    list_panel_ = column(body_);
    lv_obj_set_name_static(list_panel_, "sensors_list_panel");
    background(list_panel_, components::color_container_background);
    lv_obj_set_parent(connection_error_, list_panel_);
    list_recording_ = label(list_panel_, "", 18, amber_color);
    lv_obj_set_name_static(list_recording_, "sensors_list_recording");
    lv_obj_set_width(list_recording_, LV_PCT(100));
    lv_obj_set_style_pad_hor(list_recording_, 12, 0);
    lv_obj_set_style_max_height(list_recording_, 48, 0);
    lv_label_set_long_mode(list_recording_, LV_LABEL_LONG_DOT);
    list_ = column(list_panel_);
    lv_obj_set_name_static(list_, "sensors_list");
    lv_obj_set_height(list_, 0);
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    lv_obj_set_parent(connection_error_, list_);
    lv_obj_set_parent(list_recording_, list_);
    list_empty_ = label(list_, "No sensors on this bus.", 18, muted_color);
    lv_obj_set_name_static(list_empty_, "sensors_list_empty");
    lv_obj_set_width(list_empty_, LV_PCT(100));
    lv_obj_set_style_pad_all(list_empty_, 12, 0);

    keyboard_entry_ = plain(list_);
    lv_obj_set_name_static(keyboard_entry_, "sensor_row_keyboard");
    lv_obj_set_size(keyboard_entry_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keyboard_entry_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(keyboard_entry_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(keyboard_entry_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(keyboard_entry_, 12, 0);
    lv_obj_set_style_min_height(keyboard_entry_, 104, 0);
    separator(keyboard_entry_);
    auto *keyboard_identity = column(keyboard_entry_);
    lv_obj_remove_flag(keyboard_identity, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(keyboard_identity, 0);
    lv_obj_set_flex_grow(keyboard_identity, 1);
    lv_obj_set_style_pad_row(keyboard_identity, 4, 0);
    lv_obj_set_width(label(keyboard_identity, "TCA8418", 26), LV_PCT(100));
    lv_obj_set_width(label(keyboard_identity, "Keyboard", 18, muted_color), LV_PCT(100));
    lv_obj_set_width(label(keyboard_entry_, LV_SYMBOL_RIGHT, 18, muted_color), 24);
    lv_obj_add_event_cb(keyboard_entry_, command, LV_EVENT_CLICKED, this);

    detail_ = column(body_);
    lv_obj_set_name_static(detail_, "sensors_detail");
    background(detail_, components::color_container_background);
    lv_obj_set_style_pad_all(detail_, 16, 0);
    lv_obj_set_style_pad_row(detail_, 10, 0);
    detail_empty_ = label(detail_, "No sensor selected", 26, muted_color);
    lv_obj_set_name_static(detail_empty_, "sensors_detail_empty");
    lv_obj_set_width(detail_empty_, LV_PCT(100));
    detail_category_ = label(detail_, "", 18, muted_color);
    lv_obj_set_name_static(detail_category_, "sensors_detail_category");
    lv_obj_set_width(detail_category_, LV_PCT(100));
    detail_name_ = label(detail_, "", 32, components::color_container_title);
    lv_obj_set_name_static(detail_name_, "sensors_detail_name");
    lv_obj_set_width(detail_name_, LV_PCT(100));
    detail_status_ = label(detail_, "", 18, live_color);
    lv_obj_set_name_static(detail_status_, "sensors_detail_status");
    lv_obj_set_width(detail_status_, LV_PCT(100));
    detail_error_ = label(detail_, "", 18, fault_color);
    lv_obj_set_name_static(detail_error_, "sensors_detail_error");
    lv_obj_set_width(detail_error_, LV_PCT(100));
    readings_ = column(detail_);
    lv_obj_set_name_static(readings_, "sensors_readings");
    lv_obj_set_size(readings_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(readings_, 20, 0);
    plot_header_ = label(detail_, "LIVE HISTORY", 18, muted_color);
    plots_ = column(detail_);
    lv_obj_set_name_static(plots_, "sensors_plots");
    lv_obj_set_style_pad_row(plots_, 24, 0);
    metadata_ = label(detail_, "", 18, muted_color);
    lv_obj_set_name_static(metadata_, "sensors_metadata");
    lv_obj_set_width(metadata_, LV_PCT(100));
    lv_obj_set_style_pad_top(metadata_, 8, 0);
    recording_ = label(detail_, "", 18, amber_color);
    lv_obj_set_name_static(recording_, "sensors_recording_path");
    lv_obj_set_width(recording_, LV_PCT(100));

    buses_ = column(surface_);
    lv_obj_set_name_static(buses_, "sensors_buses");
    lv_obj_set_height(buses_, 0);
    lv_obj_set_flex_grow(buses_, 1);
    lv_obj_add_flag(buses_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(buses_, LV_DIR_VER);
    background(buses_, components::color_container_background);
    keyboard_page_ = keyboard_.create(surface_);

    tooltip_ = label(surface_, "", 18, components::color_container_background);
    lv_obj_set_name_static(tooltip_, "sensors_tooltip");
    lv_obj_add_flag(tooltip_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
    background(tooltip_, components::color_action);
    lv_obj_set_style_radius(tooltip_, 4, 0);
    lv_obj_set_style_pad_all(tooltip_, 8, 0);
    lv_obj_align(tooltip_, LV_ALIGN_TOP_RIGHT, -12, 76);
    lv_obj_add_event_cb(surface_, layout_event, LV_EVENT_SIZE_CHANGED, this);
    lv_subject_add_observer_obj(view_model_.revision_subject(), changed, surface_, this);
    timer_ = lv_timer_create(tick, 100, this);
    refresh();
    layout();
    return surface_;
}

void SensorsView::destroy()
{
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    keyboard_.destroy();
    if (surface_) lv_obj_delete(surface_);
    surface_ = header_ = status_ = pause_ = pause_icon_ = clear_ = rescan_ = nullptr;
    back_ = title_ = list_recording_ = nullptr;
    record_ = record_icon_ = tooltip_ = navigation_ = tabs_ = filter_ = interval_ = nullptr;
    count_ = connection_error_ = body_ = list_panel_ = list_ = list_empty_ = nullptr;
    detail_ = detail_name_ = detail_category_ = detail_status_ = detail_error_ = detail_empty_ = nullptr;
    readings_ = plot_header_ = plots_ = metadata_ = recording_ = buses_ = nullptr;
    keyboard_page_ = keyboard_entry_ = sampling_ = nullptr;
    rows_.clear();
    measurements_.clear();
    filters_.clear();
    row_signature_.clear();
    bus_signature_.clear();
    channel_signature_.clear();
    app_title_.clear();
    tab_ = 0;
    detail_visible_ = false;
    keyboard_visible_ = false;
}

void SensorsView::changed(lv_observer_t *observer, lv_subject_t *)
{
    static_cast<SensorsView *>(lv_observer_get_user_data(observer))->refresh();
}

void SensorsView::tick(lv_timer_t *timer)
{
    auto *self = static_cast<SensorsView *>(lv_timer_get_user_data(timer));
    self->keyboard_.tick();
    self->view_model_.tick();
    if (self->keyboard_visible_) self->refresh_keyboard_header();
}

void SensorsView::layout_event(lv_event_t *event)
{
    static_cast<SensorsView *>(lv_event_get_user_data(event))->layout();
}

void SensorsView::layout()
{
    if (!surface_ || laying_out_) return;
    laying_out_ = true;
    const bool narrow = lv_obj_get_width(surface_) < 480;
    const bool keyboard = keyboard_visible_;
    lv_obj_set_height(navigation_, 60);
    lv_obj_set_flex_flow(navigation_, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(tabs_, narrow ? LV_PCT(100) : 220);
    auto *filter_parent = narrow ? sampling_ : navigation_;
    if (lv_obj_get_parent(filter_) != filter_parent) {
        lv_obj_set_parent(filter_, filter_parent);
        if (narrow) lv_obj_move_to_index(filter_, 0);
    }
    lv_obj_set_flex_grow(filter_, 1);
    lv_obj_set_width(filter_, 0);
    hidden(filter_, keyboard || (narrow && view_model_.showing_detail()));
    hidden(count_, narrow && !view_model_.showing_detail());
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(list_panel_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_size(detail_, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(detail_, LV_DIR_VER);
    for (auto &measurement : measurements_) {
        for (auto *section : {measurement.object, measurement.plot}) {
            lv_obj_set_width(section, LV_PCT(100));
        }
        lv_obj_set_height(measurement.chart, lv_obj_get_content_height(surface_) >= 700 ? 260 : 200);
    }
    laying_out_ = false;
}

void SensorsView::refresh()
{
    if (!surface_) return;
    const auto &model = view_model_.model();
    const auto &snapshot = model.snapshot();
    const bool connected = std::any_of(snapshot.sensors.begin(), snapshot.sensors.end(),
                                      [](const auto &sensor) { return sensor.available; });
    const char *status = model.paused() ? "Paused" : snapshot.simulated ? "Demo" : connected ? "Live" : "Disconnected";
    lv_label_set_text(status_, status);
    lv_obj_set_style_text_color(status_, lv_color_hex(model.paused() ? muted_color : snapshot.simulated ? amber_color : connected ? live_color : fault_color), 0);
    lv_label_set_text(pause_icon_, model.paused() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(record_icon_, lv_color_hex(model.recording() ? fault_color : components::color_action), 0);
    lv_dropdown_set_selected(interval_, view_model_.interval() == 250 ? 1 : view_model_.interval() == 1000 ? 2 : 0);
    const auto device = model.selected_device();
    if (view_model_.showing_detail() && !device.readings.empty()) {
        std::size_t channels = 0, enabled = 0;
        for (const auto *reading : device.readings) {
            channels += reading->channels.size();
            for (std::size_t index = 0; index < reading->channels.size(); ++index)
                enabled += model.channel_recording_enabled(reading->id, index);
        }
        lv_label_set_text_fmt(count_, "%zu / %zu CSV", enabled, channels);
    } else lv_label_set_text_fmt(count_, "%zu sensors", model.visible_devices().size());
    hidden(connection_error_, snapshot.error.empty());
    lv_label_set_text(connection_error_, snapshot.error.c_str());
    std::vector<int> filters{-1, -2};
    std::string options = "All buses\nSystem";
    for (const auto &bus : snapshot.buses) {
        filters.push_back(bus.number);
        options += "\nI2C-" + std::to_string(bus.number);
    }
    if (filters != filters_) {
        filters_ = std::move(filters);
        lv_dropdown_set_options(filter_, options.c_str());
    }
    const auto position = std::find(filters_.begin(), filters_.end(), model.filter());
    lv_dropdown_set_selected(filter_, position == filters_.end() ? 0 : static_cast<uint32_t>(position - filters_.begin()));
    refresh_rows();
    refresh_detail();
    refresh_buses();
    show_tab(tab_);
    if (keyboard_visible_) refresh_keyboard_header();
}

void SensorsView::refresh_rows()
{
    const auto &model = view_model_.model();
    const auto sensors = model.visible_devices();
    std::string signature;
    for (const auto &sensor : sensors) signature += sensor.id + "\n" + sensor.name + "\n";
    if (signature != row_signature_) {
        const int scroll = lv_obj_get_scroll_y(list_);
        for (const auto &row : rows_) lv_obj_delete(row.object);
        rows_.clear();
        for (const auto &sensor : sensors) {
            Row row;
            row.id = sensor.id;
            row.object = plain(list_);
            lv_obj_set_size(row.object, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(row.object, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row.object, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_name(row.object, ("sensor_row_" + sensor.id).c_str());
            lv_obj_add_flag(row.object, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_pad_all(row.object, 12, 0);
            lv_obj_set_style_pad_row(row.object, 4, 0);
            lv_obj_set_style_min_height(row.object, 120, 0);
            separator(row.object);
            auto *content = column(row.object);
            lv_obj_set_width(content, 0);
            lv_obj_set_flex_grow(content, 1);
            lv_obj_set_style_pad_row(content, 4, 0);
            row.name = label(content, sensor.name.c_str(), 26, components::color_container_title);
            lv_obj_set_width(row.name, LV_PCT(100));
            row.status = label(content, "", 18, muted_color);
            lv_obj_set_width(row.status, LV_PCT(100));
            row.location = label(content, sensor.location.c_str(), 18, muted_color);
            lv_obj_set_width(row.location, LV_PCT(100));
            auto *arrow = label(row.object, LV_SYMBOL_RIGHT, 18, muted_color);
            lv_obj_set_width(arrow, 24);
            lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row.object, command, LV_EVENT_CLICKED, this);
            rows_.push_back(std::move(row));
        }
        row_signature_ = std::move(signature);
        lv_obj_move_to_index(keyboard_entry_, -1);
        lv_obj_update_layout(list_);
        lv_obj_scroll_to_y(list_, scroll, LV_ANIM_OFF);
    }
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const auto &sensor = sensors[index];
        auto &row = rows_[index];
        background(row.object, components::color_container_background);
        const auto address = std::filesystem::path(sensor.id).filename().string();
        const std::string context = sensor.location +
            (sensor.location != "system" && address.find('-') != std::string::npos ? " / " + address : "") +
            "  /  " + std::to_string(sensor.readings.size()) +
            (sensor.readings.size() == 1 ? " measurement" : " measurements");
        lv_label_set_text(row.location, context.c_str());
        const auto available = std::count_if(sensor.readings.begin(), sensor.readings.end(),
            [](const auto *reading) { return reading->available; });
        const bool healthy = available > 0 && static_cast<std::size_t>(available) == sensor.readings.size();
        lv_label_set_text(row.status, healthy ? "Valid" : available ? "Partially valid" : "Unavailable");
        lv_obj_set_style_text_color(row.status,
            lv_color_hex(healthy ? live_color : available ? amber_color : fault_color), 0);
    }
    hidden(list_empty_, !sensors.empty());
}

void SensorsView::refresh_detail()
{
    const auto &model = view_model_.model();
    const auto device = model.selected_device();
    hidden(detail_empty_, !device.readings.empty());
    for (auto *object : {detail_name_, detail_category_, detail_status_, readings_, plot_header_, plots_, metadata_})
        hidden(object, device.readings.empty());
    hidden(detail_error_, true);
    std::string recording_text;
    if (model.recording()) recording_text = "Recording CSV  /  " + std::to_string(model.recording_channel_count()) +
        " channels  /  " + std::to_string(model.recorded_rows()) + " rows\n" + model.recording_path();
    else if (!model.recording_error().empty()) recording_text = model.recording_error();
    else if (!model.recording_path().empty()) recording_text = "CSV saved\n" + model.recording_path();
    lv_label_set_text(recording_, recording_text.c_str());
    hidden(recording_, recording_text.empty());
    const std::string list_recording = model.recording_error().empty() && model.recording()
        ? "Recording CSV / " + std::to_string(model.recorded_rows()) + " rows" : recording_text;
    lv_label_set_text(list_recording_, list_recording.c_str());
    hidden(list_recording_, list_recording.empty());
    if (device.readings.empty() || !view_model_.showing_detail()) return;
    lv_label_set_text(detail_name_, device.name.c_str());
    lv_label_set_text(detail_category_, device.location.c_str());
    const auto available = std::count_if(device.readings.begin(), device.readings.end(),
        [](const auto *reading) { return reading->available; });
    const bool healthy = static_cast<std::size_t>(available) == device.readings.size();
    lv_label_set_text(detail_status_, model.paused() ? "Paused" : !available ? "Unavailable" :
        !healthy ? "Some readings unavailable" : model.snapshot().simulated ? "Demo data" : "Live readings");
    lv_obj_set_style_text_color(detail_status_, lv_color_hex(healthy ? live_color : fault_color), 0);
    std::string errors;
    for (const auto *reading : device.readings) {
        if (reading->error.empty()) continue;
        if (!errors.empty()) errors += "\n";
        errors += reading->name + ": " + reading->error;
    }
    lv_label_set_text(detail_error_, errors.c_str());
    hidden(detail_error_, errors.empty());
    const std::string metadata = "Device  " + device.name + "\nLocation  " + device.location + "\n" + device.path;
    lv_label_set_text(metadata_, metadata.c_str());
    std::string signature = device.id;
    for (const auto *sensor : device.readings) {
        signature += "\n" + sensor->id + ":" + sensor->name;
        signature += ":waveform=" + std::to_string(sensor->waveform.size());
        for (const auto &channel : sensor->channels) signature += "\n" + channel.name + ":" + channel.unit;
    }
    if (signature != channel_signature_) {
        lv_obj_clean(readings_);
        lv_obj_clean(plots_);
        measurements_.clear();
        std::size_t value_index = 0;
        for (const auto *sensor : device.readings) {
            Measurement measurement;
            measurement.id = sensor->id;
            auto *section = column(readings_);
            measurement.object = section;
            lv_obj_set_name(section, ("sensor_measurement_" + sensor->id).c_str());
            lv_obj_set_style_pad_row(section, 8, 0);
            lv_obj_set_width(label(section, sensor->name.c_str(), 24), LV_PCT(100));
            measurement.error = label(section, "Unavailable", 18, fault_color);
            lv_obj_set_width(measurement.error, LV_PCT(100));
            const bool waveform = sensor->category == "microphone" && sensor->name == "Raw waveform";
            auto *values = waveform ? nullptr : column(section);
            if (values) {
                lv_obj_set_flex_flow(values, LV_FLEX_FLOW_ROW_WRAP);
                lv_obj_set_style_pad_column(values, 12, 0);
                lv_obj_set_style_pad_row(values, 12, 0);
            }
            auto *plot = column(plots_);
            measurement.plot = plot;
            lv_obj_set_style_pad_row(plot, 8, 0);
            lv_obj_set_width(label(plot, sensor->name.c_str(), 24), LV_PCT(100));
            measurement.chart = lv_chart_create(plot);
            const std::string suffix = measurements_.empty() ? "" : "_" + std::to_string(measurements_.size());
            lv_obj_set_name(measurement.chart, ("sensors_chart" + suffix).c_str());
            lv_obj_set_size(measurement.chart, LV_PCT(100), 220);
            lv_obj_set_style_radius(measurement.chart, 0, 0);
            lv_obj_set_style_border_width(measurement.chart, 0, 0);
            lv_obj_set_style_pad_all(measurement.chart, 0, 0);
            lv_obj_set_style_size(measurement.chart, 0, 0, LV_PART_INDICATOR);
            lv_obj_set_style_line_width(measurement.chart, 2, LV_PART_ITEMS);
            lv_obj_set_style_line_color(measurement.chart, lv_color_hex(components::color_border), LV_PART_MAIN);
            background(measurement.chart, components::color_container_background);
            lv_chart_set_type(measurement.chart, LV_CHART_TYPE_SCATTER);
            lv_chart_set_div_line_count(measurement.chart, 5, 6);
            lv_chart_set_axis_range(measurement.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 10000);
            lv_chart_set_axis_range(measurement.chart, LV_CHART_AXIS_PRIMARY_X, 0, 10000);
            measurement.range = label(plot, "", 18, muted_color);
            lv_obj_set_name(measurement.range, ("sensors_chart_range" + suffix).c_str());
            lv_obj_set_width(measurement.range, LV_PCT(100));
            measurement.times = label(plot, "", 18, muted_color);
            lv_obj_set_name(measurement.times, ("sensors_chart_time" + suffix).c_str());
            lv_obj_set_width(measurement.times, LV_PCT(100));
            if (waveform) {
                measurement.series.push_back(lv_chart_add_series(measurement.chart,
                                                                   lv_color_hex(channel_colors[0]),
                                                                   LV_CHART_AXIS_PRIMARY_Y));
            }
            for (std::size_t index = 0; index < sensor->channels.size(); ++index, ++value_index) {
                const auto &channel = sensor->channels[index];
                const uint32_t color = channel_colors[index % 5];
                auto *group = column(values);
                lv_obj_set_width(group, sensor->channels.size() == 1 ? LV_PCT(100) : LV_PCT(30));
                lv_obj_set_style_min_width(group, 120, 0);
                lv_obj_set_flex_grow(group, 1);
                lv_obj_set_style_pad_row(group, 4, 0);
                Channel view;
                view.name = label(group, (channel.name + "  " + channel.unit).c_str(), 18, color);
                lv_obj_set_width(view.name, LV_PCT(100));
                view.value = label(group, "--", 32, color);
                lv_obj_set_name(view.value, ("sensor_value_" + std::to_string(value_index)).c_str());
                lv_obj_set_width(view.value, LV_PCT(100));
                view.extrema = label(group, "", 18, muted_color);
                lv_obj_set_name(view.extrema, ("sensor_extrema_" + std::to_string(value_index)).c_str());
                lv_obj_set_width(view.extrema, LV_PCT(100));
                view.record = lv_checkbox_create(group);
                lv_obj_set_name(view.record, ("sensor_record_channel_" + std::to_string(value_index)).c_str());
                lv_checkbox_set_text(view.record, "CSV");
                lv_obj_set_size(view.record, LV_PCT(100), 48);
                lv_obj_set_style_pad_hor(view.record, 0, 0);
                lv_obj_set_style_pad_ver(view.record, 10, 0);
                font(view.record, 18);
                lv_obj_set_style_bg_color(view.record, lv_color_hex(live_color), LV_PART_INDICATOR | LV_STATE_CHECKED);
                lv_obj_add_event_cb(view.record, command, LV_EVENT_VALUE_CHANGED, this);
                measurement.channels.push_back(view);
                measurement.series.push_back(lv_chart_add_series(measurement.chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y));
            }
            measurements_.push_back(std::move(measurement));
        }
        channel_signature_ = std::move(signature);
    }
    for (std::size_t reading = 0; reading < measurements_.size(); ++reading) {
        auto &measurement = measurements_[reading];
        const auto *sensor = device.readings[reading];
        const auto *history = model.history(sensor->id);
        hidden(measurement.error, sensor->available);
        for (std::size_t index = 0; index < measurement.channels.size(); ++index) {
            auto &channel = measurement.channels[index];
            lv_label_set_text(channel.value, sensor->available ? number(sensor->channels[index].value).c_str() : "--");
            const std::string minimum = history && index < history->minimum.size() ? number(history->minimum[index]) : "--";
            const std::string maximum = history && index < history->maximum.size() ? number(history->maximum[index]) : "--";
            const std::string extrema = "Min " + minimum + "\nMax " + maximum;
            lv_label_set_text(channel.extrema, extrema.c_str());
            lv_obj_set_state(channel.record, LV_STATE_CHECKED, model.channel_recording_enabled(sensor->id, index));
        }
        refresh_chart(measurement, *sensor);
    }
}

void SensorsView::refresh_chart(Measurement &view, const SensorReading &sensor)
{
    if (sensor.category == "microphone" && sensor.name == "Raw waveform") {
        const std::size_t size = sensor.waveform.size();
        double peak = 0;
        for (const double sample : sensor.waveform) {
            if (std::isfinite(sample)) peak = std::max(peak, std::abs(std::clamp(sample, -1.0, 1.0)));
        }
        // Quiet microphone input needs a closer Y range to remain visible.
        const double display_scale = std::clamp(peak * 1.2, 0.01, 1.0);
        lv_chart_set_point_count(view.chart, static_cast<uint32_t>(std::max<std::size_t>(size, 2)));
        if (!view.series.empty()) {
            auto *series = view.series.front();
            auto *x_values = lv_chart_get_series_x_array(view.chart, series);
            auto *y_values = lv_chart_get_series_y_array(view.chart, series);
            std::fill_n(x_values, lv_chart_get_point_count(view.chart), 0);
            std::fill_n(y_values, lv_chart_get_point_count(view.chart), LV_CHART_POINT_NONE);
            for (std::size_t index = 0; index < size; ++index) {
                const double value = std::clamp(sensor.waveform[index], -1.0, 1.0);
                x_values[index] = size > 1 ? static_cast<int32_t>(std::lround(
                    static_cast<double>(index) / static_cast<double>(size - 1) * 10000)) : 0;
                y_values[index] = static_cast<int32_t>(std::lround(
                    std::clamp((value + display_scale) / (2.0 * display_scale) * 10000.0, 0.0, 10000.0)));
            }
        }
        lv_label_set_text(view.range, (number(-display_scale) + " to " + number(display_scale) + " FS").c_str());
        char time[96];
        std::snprintf(time, sizeof(time), "0.0 - 100.0 ms   /   %zu samples", size);
        lv_label_set_text(view.times, time);
        lv_chart_refresh(view.chart);
        return;
    }
    const auto *history = view_model_.model().history(sensor.id);
    auto *chart = view.chart;
    const std::size_t size = history ? history->points.size() : 0;
    lv_chart_set_point_count(chart, static_cast<uint32_t>(std::max<std::size_t>(size, 2)));
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    if (history) {
        for (const auto &point : history->points) for (double value : point.values) {
            if (std::isfinite(value)) { low = std::min(low, value); high = std::max(high, value); }
        }
    }
    const bool data = std::isfinite(low) && std::isfinite(high);
    if (!data) { low = -1; high = 1; }
    const double padding = high > low ? (high - low) * 0.08 : std::max(std::abs(low) * 0.05, 0.1);
    low -= padding;
    high += padding;
    const double first = size ? history->points.front().seconds : 0;
    const double last = size ? history->points.back().seconds : 0;
    const double duration = std::max(last - first, 0.001);
    for (std::size_t channel = 0; channel < view.series.size(); ++channel) {
        auto *x_values = lv_chart_get_series_x_array(chart, view.series[channel]);
        auto *y_values = lv_chart_get_series_y_array(chart, view.series[channel]);
        std::fill_n(x_values, lv_chart_get_point_count(chart), 0);
        std::fill_n(y_values, lv_chart_get_point_count(chart), LV_CHART_POINT_NONE);
        for (std::size_t index = 0; index < size; ++index) {
            const auto &point = history->points[index];
            const double value = channel < point.values.size() ? point.values[channel] : std::numeric_limits<double>::quiet_NaN();
            const int32_t x = static_cast<int32_t>(std::lround((point.seconds - first) / duration * 10000));
            const int32_t y = std::isfinite(value) ? static_cast<int32_t>(std::lround((value - low) / (high - low) * 10000)) : LV_CHART_POINT_NONE;
            x_values[index] = x;
            y_values[index] = y;
        }
    }
    std::string units = !sensor.channels.empty() ? sensor.channels.front().unit : "";
    lv_label_set_text(view.range, data ? (number(low) + " to " + number(high) + " " + units).c_str() : "No samples");
    char time[96];
    std::snprintf(time, sizeof(time), "%.1f - %.1f s   /   %zu samples", first, last, size);
    lv_label_set_text(view.times, time);
    lv_chart_refresh(chart);
}

void SensorsView::refresh_buses()
{
    const auto &model = view_model_.model();
    std::ostringstream signature;
    signature << model.filter();
    for (const auto &bus : model.snapshot().buses) {
        signature << '\n' << bus.number << bus.name << bus.error;
        for (const auto &device : bus.devices)
            signature << '\n' << device.address << device.name << device.driver << static_cast<int>(device.status);
    }
    if (signature.str() == bus_signature_) return;
    bus_signature_ = signature.str();
    const int scroll = lv_obj_get_scroll_y(buses_);
    lv_obj_clean(buses_);
    bool found = false;
    for (const auto &bus : model.snapshot().buses) {
        if (model.filter() == -2 || (model.filter() >= 0 && model.filter() != bus.number)) continue;
        found = true;
        auto *section = column(buses_);
        lv_obj_set_name(section, ("sensor_bus_" + std::to_string(bus.number)).c_str());
        lv_obj_set_style_pad_all(section, 16, 0);
        lv_obj_set_style_pad_row(section, 10, 0);
        separator(section);
        auto *title = label(section, ("I2C-" + std::to_string(bus.number)).c_str(), 26, components::color_container_title);
        lv_obj_set_width(title, LV_PCT(100));
        auto *description = label(section, bus.name.c_str(), 18, muted_color);
        lv_obj_set_width(description, LV_PCT(100));
        if (!bus.error.empty()) {
            auto *error = label(section, bus.error.c_str(), 18, fault_color);
            lv_obj_set_width(error, LV_PCT(100));
        }
        for (const auto &device : bus.devices) {
            char address[16];
            std::snprintf(address, sizeof(address), "0x%02X", device.address);
            const std::string status = device.status == BusDeviceStatus::acknowledged ? "Responding / No kernel driver" :
                                       device.status == BusDeviceStatus::busy ? "Address busy" :
                                       device.driver.empty() ? "No kernel driver" : device.driver;
            const std::string text = std::string(address) + "   " + device.name + "\n" + status;
            auto *row = label(section, text.c_str());
            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_style_pad_ver(row, 6, 0);
        }
        if (bus.devices.empty()) {
            auto *empty = label(section, "No devices discovered", 18, muted_color);
            lv_obj_set_width(empty, LV_PCT(100));
        }
    }
    if (!found) {
        auto *empty = label(buses_, "No I2C buses in this selection", 26, muted_color);
        lv_obj_set_name_static(empty, "sensors_buses_empty");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_style_pad_all(empty, 16, 0);
    }
    lv_obj_update_layout(buses_);
    lv_obj_scroll_to_y(buses_, scroll, LV_ANIM_OFF);
}

void SensorsView::show_tab(unsigned tab)
{
    tab_ = std::min(tab, 1u);
    show_page();
    lv_buttonmatrix_set_button_ctrl(tabs_, tab_, LV_BUTTONMATRIX_CTRL_CHECKED);
    layout();
}

void SensorsView::show_page()
{
    const bool detail = view_model_.showing_detail();
    const bool keyboard = keyboard_visible_;
    const bool secondary = detail || keyboard;
    if (secondary != detail_visible_) {
        hidden(tooltip_, true);
        if (detail) lv_obj_scroll_to_y(detail_, 0, LV_ANIM_OFF);
        detail_visible_ = secondary;
    }
    hidden(back_, !secondary);
    hidden(clear_, !detail && !keyboard);
    hidden(rescan_, detail);
    hidden(record_, keyboard);
    hidden(filter_, keyboard);
    hidden(sampling_, keyboard);
    hidden(navigation_, secondary);
    hidden(list_panel_, secondary);
    hidden(detail_, !detail);
    hidden(body_, keyboard || (!detail && tab_ != 0));
    hidden(buses_, secondary || tab_ != 1);
    hidden(keyboard_page_, !keyboard);
    keyboard_.set_visible(keyboard);
    const auto device = view_model_.model().selected_device();
    lv_label_set_text(title_, detail && !device.readings.empty()
        ? device.name.c_str() : keyboard ? "Keyboard" : app_title_.c_str());
    if (keyboard) refresh_keyboard_header();
}

void SensorsView::refresh_keyboard_header()
{
    const auto &snapshot = keyboard_.snapshot();
    const char *status = keyboard_.paused() ? "Paused" : !snapshot.error.empty() ? "Input error" :
                         snapshot.monitoring ? "Listening" : "Disconnected";
    lv_label_set_text(status_, status);
    lv_obj_set_style_text_color(status_, lv_color_hex(keyboard_.paused() ? muted_color :
        !snapshot.error.empty() ? fault_color : snapshot.monitoring ? live_color : muted_color), 0);
    lv_label_set_text(pause_icon_, keyboard_.paused() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
}

void SensorsView::command(lv_event_t *event)
{
    auto *self = static_cast<SensorsView *>(lv_event_get_user_data(event));
    auto *target = lv_event_get_target_obj(event);
    if (self->keyboard_visible_) {
        if (target == self->back_) {
            self->keyboard_visible_ = false;
            self->refresh();
            return;
        }
        if (target == self->pause_) self->keyboard_.toggle_pause();
        else if (target == self->clear_) self->keyboard_.clear_history();
        else if (target == self->rescan_) self->keyboard_.rescan();
        if (target == self->pause_ || target == self->clear_ || target == self->rescan_) {
            self->refresh_keyboard_header();
            return;
        }
    }
    if (target == self->keyboard_entry_) {
        self->keyboard_visible_ = true;
        self->refresh();
    } else if (target == self->back_) self->view_model_.show_list();
    else if (target == self->pause_) self->view_model_.toggle_pause();
    else if (target == self->clear_) self->view_model_.clear_history();
    else if (target == self->rescan_) self->view_model_.rescan();
    else if (target == self->record_) self->view_model_.toggle_recording();
    else if (target == self->filter_) {
        const auto index = lv_dropdown_get_selected(target);
        if (index < self->filters_.size()) self->view_model_.set_filter(self->filters_[index]);
    } else if (target == self->interval_) {
        constexpr unsigned intervals[] = {100, 250, 1000};
        const auto index = lv_dropdown_get_selected(target);
        if (index < 3) self->view_model_.set_interval(intervals[index]);
    } else if (target == self->tabs_) {
        hidden(self->tooltip_, true);
        self->show_tab(lv_buttonmatrix_get_selected_button(target));
        self->refresh();
    } else {
        for (const auto &measurement : self->measurements_) {
            for (std::size_t index = 0; index < measurement.channels.size(); ++index) {
                if (target != measurement.channels[index].record) continue;
                const auto id = measurement.id;
                self->view_model_.set_channel_recording_enabled(id, index, lv_obj_has_state(target, LV_STATE_CHECKED));
                return;
            }
        }
        for (const auto &row : self->rows_) {
            if (target == row.object) {
                const std::string id = row.id;
                self->view_model_.open_sensor(id);
                break;
            }
        }
    }
}

void SensorsView::tooltip_event(lv_event_t *event)
{
    auto *self = static_cast<SensorsView *>(lv_event_get_user_data(event));
    if (!self->tooltip_) return;
    const auto code = lv_event_get_code(event);
    if (code == LV_EVENT_HOVER_OVER || code == LV_EVENT_LONG_PRESSED) {
        auto *target = lv_event_get_target_obj(event);
        const char *text = target == self->back_ ? "Back to sensors" : target == self->clear_ ? "Clear history" : target == self->rescan_ ? "Rescan devices" : target == self->record_ ? "Start / stop CSV" : "Pause / resume";
        lv_label_set_text(self->tooltip_, text);
        hidden(self->tooltip_, false);
    } else if (code == LV_EVENT_HOVER_LEAVE || code == LV_EVENT_RELEASED) hidden(self->tooltip_, true);
}

}  // namespace sensors
