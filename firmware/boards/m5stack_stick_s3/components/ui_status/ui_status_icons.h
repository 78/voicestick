#pragma once

#include "lvgl.h"

typedef enum {
    UI_STATUS_ICON_BOOT,
    UI_STATUS_ICON_PAIRING,
    UI_STATUS_ICON_IDLE,
    UI_STATUS_ICON_RESTING,
    UI_STATUS_ICON_RECORDING,
    UI_STATUS_ICON_TRANSCRIBING,
    UI_STATUS_ICON_ERROR,
} ui_status_icon_scene_t;

typedef struct {
    lv_obj_t *root;
} ui_status_icons_t;

void ui_status_icons_create(ui_status_icons_t *icons, lv_obj_t *screen);
void ui_status_icons_apply(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
void ui_status_icons_start_anim(ui_status_icons_t *icons, ui_status_icon_scene_t scene);
void ui_status_icons_stop_anim(ui_status_icons_t *icons);
