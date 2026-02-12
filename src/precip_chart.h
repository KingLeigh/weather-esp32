#pragma once
#include "epd_driver.h"
#include "firasans.h"

// Draw a precipitation probability time series line chart
// x, y: top-left corner of chart area
// w, h: width and height of chart area
// data: array of precipitation percentages (0-100)
// count: number of data points
// fb: framebuffer
static void draw_precip_chart(int32_t x, int32_t y, int32_t w, int32_t h,
                               const int *data, int count, uint8_t *fb) {
    // Use full height for chart since title is now below
    const int32_t chart_h = h;
    const int32_t chart_y = y;
    const int32_t chart_bottom = chart_y + chart_h;

    // Check if there's any precipitation
    bool has_precip = false;
    for (int i = 0; i < count; i++) {
        if (data[i] > 0) {
            has_precip = true;
            break;
        }
    }

    // Gridlines at 0%, 25%, 50%, 75% (no 100% line)
    for (int pct = 0; pct < 100; pct += 25) {
        int32_t gy = chart_bottom - (chart_h * pct / 100);
        uint8_t color = (pct == 0) ? 0xA0 : 0xC0;
        epd_draw_hline(x, gy, w, color, fb);
    }

    if (has_precip) {
        // Compute line points
        int32_t px[12], py[12];
        for (int i = 0; i < count; i++) {
            px[i] = x + (int32_t)((int64_t)i * w / (count - 1));
            py[i] = chart_bottom - (chart_h * data[i] / 100);
        }

        // Fill area under the line (column by column with light gray)
        for (int i = 0; i < count - 1; i++) {
            int32_t x0 = px[i], x1 = px[i + 1];
            int32_t y0 = py[i], y1 = py[i + 1];
            for (int32_t col = x0; col <= x1; col++) {
                int32_t top;
                if (x1 == x0) {
                    top = y0;
                } else {
                    top = y0 + (int32_t)((int64_t)(y1 - y0) * (col - x0) / (x1 - x0));
                }
                if (top < chart_bottom) {
                    epd_draw_vline(col, top, chart_bottom - top, 0xD0, fb);
                }
            }
        }

        // Draw the line segments
        for (int i = 0; i < count - 1; i++) {
            epd_draw_line(px[i], py[i], px[i + 1], py[i + 1], 0x00, fb);
            // Thicken
            epd_draw_line(px[i], py[i] + 1, px[i + 1], py[i + 1] + 1, 0x00, fb);
        }

        // Title (centered below chart)
        // Note: writeln y-coordinate is the text BASELINE, text extends upward ~20px
        const char *title = "Rain (12h)";
        // Center the text horizontally (with slight left adjustment)
        // FiraSans ~12px per character for more accurate centering
        int32_t text_width = strlen(title) * 12;
        int32_t tx = x + (w - text_width) / 2 - text_width / 4;
        // Position baseline below chart with spacing for text height + gap
        int32_t ty = chart_bottom + 45;  // 45px below chart bottom
        writeln((GFXfont *)&FiraSans, title, &tx, &ty, fb);
    }
    // else: no precipitation - show empty graph with gridlines only (no label)
}
