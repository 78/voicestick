#include "ui_status_icons.h"

#include <string.h>

#define CAT_ICON_SIZE 112
#define CAT_ICON_STRIDE (CAT_ICON_SIZE * 4)
#define CAT_ICON_DATA_SIZE (CAT_ICON_SIZE * CAT_ICON_STRIDE)
#define CAT_ICON_TOP_Y 42

extern const uint8_t cat_pairing_start[] asm("_binary_cat_pairing_argb8888_bin_start");
extern const uint8_t cat_ready_start[] asm("_binary_cat_ready_argb8888_bin_start");
extern const uint8_t cat_listening_start[] asm("_binary_cat_listening_argb8888_bin_start");
extern const uint8_t cat_thinking_start[] asm("_binary_cat_thinking_argb8888_bin_start");
extern const uint8_t cat_resting_start[] asm("_binary_cat_resting_argb8888_bin_start");
extern const uint8_t cat_error_start[] asm("_binary_cat_error_argb8888_bin_start");

static const lv_image_dsc_t s_cat_pairing = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_pairing_start,
};

static const lv_image_dsc_t s_cat_ready = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_ready_start,
};

static const lv_image_dsc_t s_cat_listening = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_listening_start,
};

static const lv_image_dsc_t s_cat_thinking = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_thinking_start,
};

static const lv_image_dsc_t s_cat_resting = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_resting_start,
};

static const lv_image_dsc_t s_cat_error = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = CAT_ICON_SIZE,
    .header.h = CAT_ICON_SIZE,
    .header.stride = CAT_ICON_STRIDE,
    .data_size = CAT_ICON_DATA_SIZE,
    .data = cat_error_start,
};

static const lv_image_dsc_t *get_scene_image(ui_status_icon_scene_t scene)
{
    switch (scene) {
    case UI_STATUS_ICON_BOOT:
    case UI_STATUS_ICON_PAIRING:
        return &s_cat_pairing;
    case UI_STATUS_ICON_IDLE:
        return &s_cat_ready;
    case UI_STATUS_ICON_RESTING:
        return &s_cat_resting;
    case UI_STATUS_ICON_RECORDING:
        return &s_cat_listening;
    case UI_STATUS_ICON_TRANSCRIBING:
        return &s_cat_thinking;
    case UI_STATUS_ICON_ERROR:
        return &s_cat_error;
    }
    return &s_cat_ready;
}

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen)
{
    memset(icons, 0, sizeof(*icons));

    icons->root = lv_image_create(screen);
    lv_obj_remove_style_all(icons->root);
    lv_image_set_src(icons->root, &s_cat_pairing);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, CAT_ICON_TOP_Y);
}

void ui_status_icons_stop_anim(ui_status_icons_t *icons)
{
    lv_anim_delete(icons->root, NULL);
}

void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    ui_status_icons_stop_anim(icons);
    lv_image_set_src(icons->root, get_scene_image(scene));
    lv_obj_set_style_opa(icons->root, LV_OPA_COVER, 0);
    lv_obj_align(icons->root, LV_ALIGN_TOP_MID, 0, CAT_ICON_TOP_Y);
}

void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene)
{
    (void)icons;
    (void)scene;
}
