#pragma once
#include "epd_driver.h"
#include "firasans.h"

// Draw a precipitation probability bar chart
// x, y: top-left corner of chart area
// w, h: width and height of chart area
// data: array of 12 precipitation percentages (0-100)
// fb: framebuffer
static void draw_precip_chart(int32_t x, int32_t y, int32_t w, int32_t h,
                               const int *data, int count, uint8_t *fb) {
    const int32_t label_h = 30;   // space for hour labels at bottom
    const int32_t title_h = 25;   // space for title at top
    const int32_t chart_h = h - label_h - title_h;
    const int32_t chart_y = y + title_h;
    const int32_t bar_gap = 8;
    const int32_t bar_w = (w - bar_gap * (count + 1)) / count;

    // Title
    const char *title = "Precipitation next 12h";
    int32_t tx = x;
    int32_t ty = y + 20;
    writeln((GFXfont *)&FiraSans, title, &tx, &ty, fb);

    // Horizontal gridlines at 25%, 50%, 75%
    uint8_t grid_color = 0xC0;  // light gray
    for (int pct = 25; pct <= 75; pct += 25) {
        int32_t gy = chart_y + chart_h - (chart_h * pct / 100);
        epd_draw_hline(x, gy, w, grid_color, fb);
    }

    // Draw bars
    for (int i = 0; i < count; i++) {
        int32_t bar_h = chart_h * data[i] / 100;
        int32_t bx = x + bar_gap + i * (bar_w + bar_gap);
        int32_t by = chart_y + chart_h - bar_h;

        if (bar_h > 0) {
            epd_fill_rect(bx, by, bar_w, bar_h, 0x00, fb);
        }

        // Hour label below
        char label[4];
        int hour = (12 + i) % 12;
        if (hour == 0) hour = 12;
        snprintf(label, sizeof(label), "%d", hour);

        int32_t lx = bx + bar_w / 2 - 5;
        int32_t ly = chart_y + chart_h + 22;
        writeln((GFXfont *)&FiraSans, label, &lx, &ly, fb);
    }
}
