#include "components/components.hpp"

#include <cm0/typography.h>

namespace components {

lv_obj_t *create_page(lv_obj_t *parent)
{
    auto *page = lv_obj_create(parent);
    lv_obj_set_name_static(page, "app_surface");
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(color_page_background), 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    return page;
}

lv_obj_t *create_header(lv_obj_t *parent, const char *title_text)
{
    auto *header = lv_obj_create(parent);
    lv_obj_set_name_static(header, "app_header");
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 88);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, lv_color_hex(color_border), 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(color_container_background), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(header, 28, 0);
    lv_obj_set_style_pad_ver(header, 18, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    auto *title = lv_label_create(header);
    lv_obj_set_name_static(title, "app_title");
    lv_label_set_text(title, title_text);
    lv_obj_set_style_text_font(title, lilygo_ui_font_get(36), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(color_container_title), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    return header;
}

lv_obj_t *create_card(lv_obj_t *parent)
{
    auto *card = lv_obj_create(parent);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(color_border), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(color_container_background), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *create_button(lv_obj_t *parent, const char *text,
                        lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_height(button, 56);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color_action), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    auto *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lilygo_ui_font_get(22), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color_container_background), 0);
    lv_obj_center(label);
    return button;
}

}  // namespace components
