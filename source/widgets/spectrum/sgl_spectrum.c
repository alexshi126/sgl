/* source/widgets/spectrum/sgl_spectrum.c
 *
 * MIT License
 *
 * Copyright(c) 2023-present All contributors of SGL
 * Document reference link: docs directory
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <sgl_core.h>
#include <sgl_draw.h>
#include <sgl_math.h>
#include <sgl_log.h>
#include <sgl_mm.h>
#include <sgl_theme.h>
#include <sgl_cfgfix.h>
#include <string.h>
#include "sgl_spectrum.h"

static void sgl_spectrum_layout(sgl_spectrum_t *spectrum)
{
    sgl_obj_t *obj = &spectrum->obj;
    int16_t w = sgl_obj_get_width(obj);
    int16_t n = (int16_t)spectrum->bar_num;
    int16_t gap = 2;
    int16_t widths[SGL_SPECTRUM_BAR_MAX];
    int16_t x;

    if (n <= 0 || w <= 0) {
        spectrum->bar_width = 0;
        spectrum->bar_gap   = 0;
        return;
    }

    /* pick the largest gap that still leaves at least 1 px per bar */
    while (gap > 1 && (w - (n - 1) * gap) / n < 1) {
        gap--;
    }

    /* reuse sgl_split_len_avg: evenly split w into n bars separated by
     * gap, with Bresenham error distribution for the remainder pixels */
    sgl_split_len_avg(w, n, gap, widths);

    spectrum->bar_gap    = (uint8_t)gap;
    spectrum->bar_width  = (uint8_t)widths[0];
    spectrum->bar_height = (uint16_t)sgl_obj_get_height(obj);

    /* cache the left x of every bar: sum of the previous split lengths
     * plus the gaps in between */
    x = obj->coords.x1 + gap / 2;   /* center the row */
    for (int i = 0; i < n; i++) {
        spectrum->bar_x[i] = x;
        x += widths[i] + gap;
    }
}

/* x of the left edge of bar i (precomputed by sgl_spectrum_layout) */
static inline int16_t sgl_spectrum_bar_x(sgl_spectrum_t *spectrum, int i)
{
    return spectrum->bar_x[i];
}

static void sgl_spectrum_construct_cb(sgl_surf_t *surf, sgl_obj_t *obj, sgl_event_t *evt)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    sgl_area_t rect;
    sgl_area_t rect_hat;

    if (evt->type != SGL_EVENT_DRAW_MAIN || spectrum->bar_value == NULL) {
        return;
    }

    for (int i = 0; i < spectrum->bar_num; i++) {
        int16_t x = sgl_spectrum_bar_x(spectrum, i);
        int16_t y2 = obj->coords.y2;
        int16_t y1 = y2 - (int16_t)spectrum->bar_value[i];

        rect.x1 = x;
        rect.x2 = x + spectrum->bar_width - 1;

        if (spectrum->bar_mode & SGL_SPECTRUM_MODE_BAR) {
            rect.y1 = y1;
            rect.y2 = y2;
            if (rect.y2 >= rect.y1) {
                sgl_draw_fill_rect(surf, &obj->area, &rect, 0,
                                   spectrum->bar_color, spectrum->alpha);
            }
        }
        else { /* SGL_SPECTRUM_MODE_BLOCK */
            int16_t step = spectrum->bar_hat_height + 1;
            rect.y2 = y2;
            while (rect.y2 > y1) {
                rect.y1 = rect.y2 - spectrum->bar_hat_height + 1;
                sgl_draw_fill_rect(surf, &obj->area, &rect, 0,
                                   spectrum->bar_color, spectrum->alpha);
                rect.y2 = rect.y1 - 1;
            }
        }

        if (spectrum->bar_mode & SGL_SPECTRUM_MODE_HAT_FLAG) {
            int16_t hy = obj->coords.y2 - (int16_t)spectrum->bar_hat[i];
            rect_hat.x1 = rect.x1;
            rect_hat.x2 = rect.x2;
            rect_hat.y1 = hy;
            rect_hat.y2 = hy + spectrum->bar_hat_height - 1;
            sgl_draw_fill_rect(surf, &obj->area, &rect_hat, 0,
                               spectrum->bar_hat_color, spectrum->alpha);
        }
    }
}

/**
 * @brief create a spectrum object
 * @param parent parent of the spectrum
 * @return spectrum object
 */
sgl_obj_t* sgl_spectrum_create(sgl_obj_t* parent)
{
    sgl_spectrum_t *spectrum = sgl_malloc(sizeof(sgl_spectrum_t));
    if (spectrum == NULL) {
        SGL_LOG_ERROR("sgl_spectrum_create: malloc failed");
        return NULL;
    }

    /* set object all member to zero */
    memset(spectrum, 0, sizeof(sgl_spectrum_t));

    sgl_obj_t *obj = &spectrum->obj;
    sgl_obj_init(&spectrum->obj, parent);
    obj->construct_fn = sgl_spectrum_construct_cb;

    spectrum->alpha = SGL_THEME_ALPHA;
    spectrum->bar_color = SGL_THEME_BG_COLOR;
    spectrum->bar_hat_color = sgl_color_mixer(SGL_THEME_BG_COLOR, SGL_THEME_COLOR, 128);
    spectrum->bar_mode = SGL_SPECTRUM_MODE_BLOCK;
    spectrum->bar_num = 0;
    spectrum->bar_width = 0;
    spectrum->bar_gap = 0;
    spectrum->bar_hat_height = 3;

    return obj;
}

/**
 * @brief set spectrum bar number
 * @param obj spectrum object
 * @param number bar number
 * @return none
 * @note re-layouts the bars and (re)allocates the value/hat buffers; safe
 *       to call again with a different number.
 */
void sgl_spectrum_set_bar_number(sgl_obj_t *obj, uint16_t number)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);

    /* clamp to the fixed buffer size */
    if (number > SGL_SPECTRUM_BAR_MAX) {
        number = SGL_SPECTRUM_BAR_MAX;
    }

    spectrum->bar_num = number;
    memset(spectrum->bar_value, 0, sizeof(spectrum->bar_value));
    memset(spectrum->bar_hat, 0, sizeof(spectrum->bar_hat));

    sgl_spectrum_layout(spectrum);
    sgl_obj_set_dirty(obj);
}

/**
 * @brief set spectrum bar value
 * @param obj spectrum object
 * @param index bar index
 * @param value bar value, 0..widget height in px (clamped)
 * @return none
 */
void sgl_spectrum_set_bar_value(sgl_obj_t *obj, uint16_t index, uint16_t value)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    int16_t old;
    int16_t new;
    int16_t top;
    int16_t hat_top;
    sgl_area_t area;

    if (spectrum->bar_value == NULL || index >= spectrum->bar_num) {
        return;
    }

    /* clamp: value is a pixel height, cannot exceed the widget inner height */
    if (value > spectrum->bar_height) {
        value = spectrum->bar_height;
    }

    old = (int16_t)spectrum->bar_value[index];
    new = (int16_t)value;
    if (new == old) {
        return;
    }

    top = obj->coords.y2 - spectrum->bar_height;   /* full-scale line */

    /* dirty region = union of the old and new bar rects (+ hat) */
    area.x1 = sgl_spectrum_bar_x(spectrum, index);
    area.x2 = area.x1 + spectrum->bar_width - 1;
    area.y1 = sgl_min(obj->coords.y2 - new, obj->coords.y2 - old);
    area.y2 = obj->coords.y2;

    if (spectrum->bar_mode & SGL_SPECTRUM_MODE_HAT_FLAG) {
        /* hat falls by bar_hat_height per update, may cover more area */
        hat_top = obj->coords.y2 - (int16_t)spectrum->bar_hat[index];
        area.y1 = sgl_min(area.y1, hat_top);
    }

    area.y1 = sgl_max(area.y1, top);
    spectrum->bar_value[index] = value;

    if (spectrum->bar_mode & SGL_SPECTRUM_MODE_HAT_FLAG) {
        spectrum->bar_hat[index] = sgl_max(value, spectrum->bar_hat[index] - spectrum->bar_hat_height);
    }

    sgl_update_area(&area);
}

/**
 * @brief set spectrum bar mode
 * @param obj spectrum object
 * @param mode bar mode
 * @return none
 * @note mode value:
 *       SGL_SPECTRUM_MODE_BAR: bar mode
 *       SGL_SPECTRUM_MODE_BLOCK: block mode
 *       SGL_SPECTRUM_MODE_BAR_HAT: bar mode with hat
 *       SGL_SPECTRUM_MODE_BLOCK_HAT: block mode with hat
 */
void sgl_spectrum_set_bar_mode(sgl_obj_t *obj, uint8_t mode)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);

    spectrum->bar_mode = mode;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief set spectrum bar color
 * @param obj spectrum object
 * @param color bar color
 * @return none
 */
void sgl_spectrum_set_bar_color(sgl_obj_t *obj, sgl_color_t color)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    spectrum->bar_color = color;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief set spectrum bar hat color
 * @param obj spectrum object
 * @param color bar hat color
 * @return none
 */
void sgl_spectrum_set_bar_hat_color(sgl_obj_t *obj, sgl_color_t color)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    spectrum->bar_hat_color = color;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief set spectrum bar hat height
 * @param obj spectrum object
 * @param height bar hat height
 * @return none
 */
void sgl_spectrum_set_bar_hat_height(sgl_obj_t *obj, uint8_t height)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    spectrum->bar_hat_height = height;
    sgl_obj_set_dirty(obj);
}

/**
 * @brief set spectrum alpha
 * @param obj spectrum object
 * @param alpha alpha value
 * @return none
 */
void sgl_spectrum_set_alpha(sgl_obj_t *obj, uint8_t alpha)
{
    sgl_spectrum_t *spectrum = sgl_container_of(obj, sgl_spectrum_t, obj);
    spectrum->alpha = alpha;
    sgl_obj_set_dirty(obj);
}
