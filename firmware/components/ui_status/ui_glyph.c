/*
 * Gezeichnete Symbole (Paket J) -- was sie sind und warum, steht in ui_glyph.h.
 *
 * Art und Zustand liegen im user_data des Objekts (Art im unteren Byte,
 * "an" in Bit 8), die Farbe in seiner Textfarbe. Gezeichnet wird im
 * LVGL-Strang mit gehaltener Sperre, wie alles andere an der Anzeige.
 */
#include "ui_glyph.h"

#define GLYPH_ON_BIT 0x100u

/* Rasterwert (0..24) auf das Objekt umgerechnet. */
static inline int32_t sc(int32_t v, int32_t size)
{
    return v * size / 24;
}

static void draw_circle(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r, bool fill,
                        lv_color_t colour, int32_t border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = colour;
    d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    d.border_color = colour;
    d.border_width = fill ? 0 : border;
    d.border_opa = fill ? LV_OPA_TRANSP : LV_OPA_COVER;
    const lv_area_t area = { .x1 = cx - r, .y1 = cy - r, .x2 = cx + r, .y2 = cy + r };
    lv_draw_rect(layer, &d, &area);
}

static void draw_box(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                     int32_t radius, bool fill, lv_color_t colour, int32_t border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    d.bg_color = colour;
    d.bg_opa = fill ? LV_OPA_COVER : LV_OPA_TRANSP;
    d.border_color = colour;
    d.border_width = fill ? 0 : border;
    d.border_opa = fill ? LV_OPA_TRANSP : LV_OPA_COVER;
    const lv_area_t area = { .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2 };
    lv_draw_rect(layer, &d, &area);
}

static void draw_line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      int32_t width, lv_color_t colour)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = colour;
    d.width = width;
    d.opa = LV_OPA_COVER;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    lv_draw_line(layer, &d);
}

static void draw_triangle(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                          int32_t x3, int32_t y3, lv_color_t colour)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.bg_color = colour;
    d.bg_opa = LV_OPA_COVER;
    d.p[0].x = x1;
    d.p[0].y = y1;
    d.p[1].x = x2;
    d.p[1].y = y2;
    d.p[2].x = x3;
    d.p[2].y = y3;
    lv_draw_triangle(layer, &d);
}

static void draw_arc_top(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t r, int32_t width,
                         lv_color_t colour)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = colour;
    d.width = width;
    d.opa = LV_OPA_COVER;
    d.rounded = 1;
    /* LVGL zaehlt im Uhrzeigersinn ab 3 Uhr: 180 -> 360 ist der obere Halbkreis. */
    d.start_angle = 180;
    d.end_angle = 360;
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)r;
    lv_draw_arc(layer, &d);
}

static void glyph_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    const uintptr_t data = (uintptr_t)lv_obj_get_user_data(obj);
    const ui_glyph_t kind = (ui_glyph_t)(data & 0xffu);
    const bool on = (data & GLYPH_ON_BIT) != 0;
    if (!layer || kind == UI_GLYPH_NONE) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    const int32_t s = w < h ? w : h;
    const int32_t x0 = a.x1 + (w - s) / 2;
    const int32_t y0 = a.y1 + (h - s) / 2;
    const lv_color_t colour = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    const lv_color_t grey = lv_color_hex(UI_GLYPH_OFF_COLOUR);
    const lv_color_t ink = on ? colour : grey;
    const lv_color_t black = lv_color_hex(0x000000);
    int32_t stroke = s / 10;
    if (stroke < 2) {
        stroke = 2;
    }
#define X(v) (x0 + sc((v), s))
#define Y(v) (y0 + sc((v), s))
    switch (kind) {
    case UI_GLYPH_BACK:
        draw_line(layer, X(15), Y(5), X(8), Y(12), stroke + 1, colour);
        draw_line(layer, X(8), Y(12), X(15), Y(19), stroke + 1, colour);
        break;
    case UI_GLYPH_FORWARD:
        draw_line(layer, X(9), Y(5), X(16), Y(12), stroke + 1, colour);
        draw_line(layer, X(16), Y(12), X(9), Y(19), stroke + 1, colour);
        break;
    case UI_GLYPH_LIGHT:
        /* Kolben und Sockel: an gefuellt in der Farbe, aus nur der Umriss. */
        draw_circle(layer, X(12), Y(10), sc(7, s), on, ink, stroke);
        draw_box(layer, X(9), Y(18), X(15), Y(21), sc(1, s), true, ink, 0);
        break;
    case UI_GLYPH_SWITCH:
        /* Kippschalter: Knopf rechts auf gefuellter Bahn = an, links im Umriss = aus. */
        draw_box(layer, X(2), Y(7), X(22), Y(17), LV_RADIUS_CIRCLE, on, ink, stroke);
        draw_circle(layer, on ? X(17) : X(7), Y(12), sc(3, s), true, on ? black : grey, 0);
        break;
    case UI_GLYPH_FAN:
        /* Drei Fluegel um die Nabe. */
        draw_circle(layer, X(12), Y(6), sc(4, s), on, ink, stroke);
        draw_circle(layer, X(7), Y(15), sc(4, s), on, ink, stroke);
        draw_circle(layer, X(17), Y(15), sc(4, s), on, ink, stroke);
        draw_circle(layer, X(12), Y(12), sc(2, s), true, ink, 0);
        break;
    case UI_GLYPH_LOCK:
        /* Offen: Buegel hochgezogen, rechts frei; zu: Buegel sitzt auf dem Koerper. */
        if (on) {
            draw_arc_top(layer, X(12), Y(7), sc(5, s), stroke, ink);
            draw_line(layer, X(7), Y(7), X(7), Y(11), stroke, ink);
        } else {
            draw_arc_top(layer, X(12), Y(11), sc(5, s), stroke, ink);
        }
        draw_box(layer, X(5), Y(11), X(19), Y(21), sc(3, s), on, ink, stroke);
        break;
    case UI_GLYPH_LIST:
        draw_line(layer, X(5), Y(7), X(19), Y(7), stroke, colour);
        draw_line(layer, X(5), Y(12), X(19), Y(12), stroke, colour);
        draw_line(layer, X(5), Y(17), X(19), Y(17), stroke, colour);
        break;
    case UI_GLYPH_SILENT:
        /* Lautsprecher (Kasten und Trichter) mit Kreuz daneben. */
        draw_box(layer, X(2), Y(9), X(6), Y(15), 0, true, colour, 0);
        draw_triangle(layer, X(6), Y(9), X(12), Y(4), X(12), Y(20), colour);
        draw_triangle(layer, X(6), Y(9), X(12), Y(20), X(6), Y(15), colour);
        draw_line(layer, X(15), Y(9), X(21), Y(15), stroke, colour);
        draw_line(layer, X(21), Y(9), X(15), Y(15), stroke, colour);
        break;
    case UI_GLYPH_STILL:
        /* Mondsichel: ein Kreis, von einem schwarzen angeschnitten (der Grund ist schwarz). */
        draw_circle(layer, X(11), Y(12), sc(8, s), true, colour, 0);
        draw_circle(layer, X(16), Y(9), sc(7, s), true, black, 0);
        break;
    case UI_GLYPH_NONE:
        break;
    }
#undef X
#undef Y
}

lv_obj_t *ui_glyph_create(lv_obj_t *parent, int32_t size)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_user_data(obj, (void *)(uintptr_t)UI_GLYPH_NONE);
    lv_obj_add_event_cb(obj, glyph_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    return obj;
}

void ui_glyph_set(lv_obj_t *glyph, ui_glyph_t kind, bool on, uint32_t colour)
{
    if (!glyph) {
        return;
    }
    if (kind == UI_GLYPH_NONE) {
        lv_obj_add_flag(glyph, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const uintptr_t data = (uintptr_t)kind | (on ? GLYPH_ON_BIT : 0u);
    const lv_color_t c = lv_color_hex(colour);
    const lv_color_t old = lv_obj_get_style_text_color(glyph, LV_PART_MAIN);
    if ((uintptr_t)lv_obj_get_user_data(glyph) != data || !lv_color_eq(old, c)) {
        lv_obj_set_user_data(glyph, (void *)data);
        lv_obj_set_style_text_color(glyph, c, 0);
        lv_obj_invalidate(glyph);
    }
    lv_obj_remove_flag(glyph, LV_OBJ_FLAG_HIDDEN);
}
