#pragma once

#include "pages/keyboard/keyboard_source.hpp"

#include <lvgl.h>

namespace sensors {

class KeyboardView {
public:
    explicit KeyboardView(std::unique_ptr<KeyboardSource> source) : source_(std::move(source)) {}
    lv_obj_t *create(lv_obj_t *parent);
    void destroy();
    void set_visible(bool visible);
    void tick();
    void toggle_pause();
    void clear_history();
    void rescan();
    bool paused() const { return paused_; }
    const KeyboardSnapshot &snapshot() const { return snapshot_; }

private:
    static void select_event(lv_event_t *event);
    static void layout_event(lv_event_t *event);
    void refresh();
    void layout();
    std::unique_ptr<KeyboardSource> source_;
    KeyboardSnapshot snapshot_;
    lv_obj_t *page_ = nullptr, *devices_ = nullptr, *device_info_ = nullptr, *error_ = nullptr;
    lv_obj_t *last_title_ = nullptr, *last_key_ = nullptr, *last_code_ = nullptr, *last_state_ = nullptr;
    lv_obj_t *pressed_ = nullptr, *events_ = nullptr, *empty_ = nullptr, *event_count_ = nullptr;
    std::vector<std::string> device_ids_;
    std::string device_options_;
    unsigned long long event_origin_ms_ = 0;
    bool visible_ = false, paused_ = false;
};

}  // namespace sensors
