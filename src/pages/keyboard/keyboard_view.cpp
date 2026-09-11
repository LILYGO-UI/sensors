#include "pages/keyboard/keyboard_view.hpp"
#include "components/components.hpp"

#include <cm0/typography.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace sensors {
namespace {
constexpr uint32_t muted = 0x657078, live = 0x087f78, fault = 0xc43d4e;

lv_obj_t *text(lv_obj_t *parent, const char *name, const char *value, int size = 18)
{
    auto *label = lv_label_create(parent);
    lv_obj_set_name(label, name);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, value);
    if (const auto *font = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(components::color_text), 0);
    return label;
}

void hide(lv_obj_t *object, bool hidden)
{
    lv_obj_set_flag(object, LV_OBJ_FLAG_HIDDEN, hidden);
}

const char *action(int value) { return value == 0 ? "Up" : value == 2 ? "Repeat" : "Down"; }
}  // namespace

lv_obj_t *KeyboardView::create(lv_obj_t *parent)
{
    page_ = lv_obj_create(parent);
    lv_obj_remove_style_all(page_);
    lv_obj_set_name_static(page_, "keyboard_page");
    lv_obj_set_width(page_, LV_PCT(100));
    lv_obj_set_height(page_, 0);
    lv_obj_set_flex_grow(page_, 1);
    lv_obj_set_flex_flow(page_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page_, 16, 0);
    lv_obj_set_style_pad_row(page_, 10, 0);
    lv_obj_set_style_bg_color(page_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(page_, LV_OPA_COVER, 0);
    lv_obj_set_scroll_dir(page_, LV_DIR_VER);

    devices_ = lv_dropdown_create(page_);
    lv_obj_set_name_static(devices_, "keyboard_devices");
    lv_obj_set_size(devices_, LV_PCT(100), 48);
    if (const auto *font = lilygo_ui_font_get(18)) {
        auto *list = lv_dropdown_get_list(devices_);
        lv_obj_set_style_text_font(devices_, font, 0);
        lv_obj_set_style_text_font(list, font, 0);
        const auto line_space = std::max(0, 48 - static_cast<int>(font->line_height));
        lv_obj_set_style_text_line_space(list, line_space, 0);
        lv_obj_set_style_text_line_space(list, line_space, LV_PART_SELECTED);
    }
    lv_obj_add_event_cb(devices_, select_event, LV_EVENT_VALUE_CHANGED, this);
    device_info_ = text(page_, "keyboard_device_info", "");
    lv_obj_set_style_text_color(device_info_, lv_color_hex(muted), 0);
    error_ = text(page_, "keyboard_error", "");
    lv_obj_set_style_text_color(error_, lv_color_hex(fault), 0);

    last_title_ = text(page_, "keyboard_last_title", "LAST EVENT");
    last_key_ = text(page_, "keyboard_last_key", "--", 36);
    last_code_ = text(page_, "keyboard_last_code", "--");
    last_state_ = text(page_, "keyboard_last_state", "Waiting");
    text(page_, "keyboard_pressed_title", "HELD KEYS");
    pressed_ = text(page_, "keyboard_pressed", "None", 26);
    event_count_ = text(page_, "keyboard_event_count", "EVENTS / 0");
    lv_obj_set_style_pad_top(event_count_, 12, 0);
    empty_ = text(page_, "keyboard_empty", "No key events");
    lv_obj_set_style_text_color(empty_, lv_color_hex(muted), 0);
    events_ = lv_table_create(page_);
    lv_obj_set_name_static(events_, "keyboard_events");
    lv_obj_set_width(events_, LV_PCT(100));
    lv_obj_set_style_border_width(events_, 0, 0);
    lv_obj_set_style_pad_all(events_, 0, 0);
    lv_obj_set_style_pad_hor(events_, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(events_, 12, LV_PART_ITEMS);
    if (const auto *font = lilygo_ui_font_get(18)) lv_obj_set_style_text_font(events_, font, LV_PART_ITEMS);
    lv_table_set_column_count(events_, 3);
    lv_obj_remove_flag(events_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(events_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(page_, layout_event, LV_EVENT_SIZE_CHANGED, this);
    refresh();
    return page_;
}

void KeyboardView::destroy()
{
    if (source_) source_->set_active(false);
    visible_ = false;
    paused_ = false;
    page_ = devices_ = device_info_ = error_ = last_title_ = last_key_ = last_code_ = last_state_ = nullptr;
    pressed_ = events_ = empty_ = event_count_ = nullptr;
    device_ids_.clear();
    device_options_.clear();
    snapshot_ = {};
    event_origin_ms_ = 0;
}

void KeyboardView::set_visible(bool visible)
{
    if (visible_ == visible) return;
    visible_ = visible;
    if (source_) source_->set_active(visible && !paused_);
    if (visible) tick();
}

void KeyboardView::tick()
{
    if (!source_ || !page_ || !visible_) return;
    auto next = source_->snapshot();
    if (next.sequence == snapshot_.sequence) return;
    if (next.selected_id != snapshot_.selected_id || next.events.empty()) event_origin_ms_ = 0;
    snapshot_ = std::move(next);
    refresh();
}

void KeyboardView::toggle_pause()
{
    paused_ = !paused_;
    if (source_) source_->set_active(visible_ && !paused_);
    tick();
}

void KeyboardView::clear_history()
{
    if (source_) source_->clear_history();
    tick();
}

void KeyboardView::rescan()
{
    if (source_) source_->rescan();
    tick();
}

void KeyboardView::select_event(lv_event_t *event)
{
    auto *self = static_cast<KeyboardView *>(lv_event_get_user_data(event));
    const auto index = lv_dropdown_get_selected(self->devices_);
    if (self->source_ && index < self->device_ids_.size()) {
        self->source_->select_device(self->device_ids_[index]);
        self->tick();
    }
}

void KeyboardView::layout_event(lv_event_t *event)
{
    static_cast<KeyboardView *>(lv_event_get_user_data(event))->layout();
}

void KeyboardView::layout()
{
    if (!events_) return;
    const bool compact = lv_obj_get_height(lv_obj_get_parent(page_)) <= 360;
    lv_obj_set_style_pad_all(page_, compact ? 8 : 16, 0);
    lv_obj_set_style_pad_row(page_, compact ? 4 : 10, 0);
    hide(last_title_, compact);
    hide(device_info_, compact || std::string(lv_label_get_text(device_info_)).empty());
    const auto width = std::max(240, static_cast<int>(lv_obj_get_content_width(page_)));
    lv_table_set_column_width(events_, 0, 88);
    lv_table_set_column_width(events_, 1, width - 168);
    lv_table_set_column_width(events_, 2, 80);
}

void KeyboardView::refresh()
{
    if (!page_) return;
    std::string options;
    device_ids_.clear();
    unsigned selected = 0;
    std::string info;
    for (const auto &device : snapshot_.devices) {
        if (!options.empty()) options += '\n';
        options += device.name + " / " + std::filesystem::path(device.path).filename().string();
        if (device.id == snapshot_.selected_id) {
            selected = static_cast<unsigned>(device_ids_.size());
            info = device.location + " / " + device.path;
        }
        device_ids_.push_back(device.id);
    }
    if (options.empty()) options = "No TCA8418 keyboard";
    if (options != device_options_) {
        lv_dropdown_set_options(devices_, options.c_str());
        device_options_ = options;
    }
    lv_dropdown_set_selected(devices_, selected);
    lv_obj_set_state(devices_, LV_STATE_DISABLED, device_ids_.empty());
    lv_label_set_text(device_info_, info.c_str());
    hide(device_info_, info.empty());
    lv_label_set_text(error_, snapshot_.error.c_str());
    hide(error_, snapshot_.error.empty());
    const auto *last = snapshot_.events.empty() ? nullptr : &snapshot_.events.back();
    lv_label_set_text(last_key_, last ? last->name.c_str() : "--");
    if (last) lv_label_set_text_fmt(last_code_, "Code %u / 0x%03X", last->code, last->code);
    else lv_label_set_text(last_code_, "--");
    lv_label_set_text(last_state_, last ? action(last->value) : "Waiting");
    lv_obj_set_style_text_color(last_state_, lv_color_hex(last && last->value != 0 ? live : muted), 0);
    std::string held;
    for (const auto code : snapshot_.pressed) {
        if (!held.empty()) held += " + ";
        held += key_name(code);
    }
    lv_label_set_text(pressed_, held.empty() ? "None" : held.c_str());
    lv_label_set_text_fmt(event_count_, "EVENTS / %zu", snapshot_.events.size());
    hide(empty_, last != nullptr);
    hide(events_, last == nullptr);
    lv_table_set_row_count(events_, static_cast<uint32_t>(snapshot_.events.size() + 1));
    lv_table_set_cell_value(events_, 0, 0, "Time (s)");
    lv_table_set_cell_value(events_, 0, 1, "Key / Code");
    lv_table_set_cell_value(events_, 0, 2, "State");
    unsigned row = 1;
    if (!snapshot_.events.empty() && !event_origin_ms_) event_origin_ms_ = snapshot_.events.front().timestamp_ms;
    for (auto it = snapshot_.events.rbegin(); it != snapshot_.events.rend(); ++it, ++row) {
        char time[40];
        const auto elapsed = it->timestamp_ms >= event_origin_ms_ ? it->timestamp_ms - event_origin_ms_ : 0;
        std::snprintf(time, sizeof(time), "%.3f", elapsed / 1000.0);
        lv_table_set_cell_value(events_, row, 0, time);
        lv_table_set_cell_value(events_, row, 1, (it->name + " / " + std::to_string(it->code)).c_str());
        lv_table_set_cell_value(events_, row, 2, action(it->value));
    }
    layout();
}

}  // namespace sensors
