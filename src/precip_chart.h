#pragma once
#include <time.h>
#include "epd_driver.h"
#include "firasans.h"

// Draw a dotted vertical time marker line on a chart
// target_hour: the hour of day to mark (0=midnight, 6=6am, 12=noon, 18=6pm)
// thickness: number of parallel lines (1=thin, 3=thick)
static void draw_time_marker(int target_hour, int current_hour, int count,
                              int32_t chart_x, int32_t chart_w, int32_t chart_y, int32_t chart_bottom,
                              uint8_t color, int thickness, uint8_t *fb) {
    int hours_offset = (target_hour - current_hour + 24) % 24;
    if (hours_offset == 0 || hours_offset >= count) return;

    int32_t marker_x = chart_x + (int32_t)((int64_t)hours_offset * chart_w / (count - 1));
    int offset = thickness / 2;
    for (int32_t dy = chart_y; dy < chart_bottom; dy += 8) {
        for (int i = -offset; i <= offset; i++) {
            epd_draw_vline(marker_x + i, dy, 5, color, fb);
        }
    }
}

// Draw a precipitation probability time series line chart
// x, y: top-left corner of chart area
// w, h: width and height of chart area
// data: array of precipitation percentages (0-100)
// count: number of data points
// precip_type: "rain", "snow", or "mixed"
// fb: framebuffer
static void draw_precip_chart(int32_t x, int32_t y, int32_t w, int32_t h,
                               const int *data, int count, const char *precip_type, uint8_t *fb) {
    // Get current hour to calculate midnight position
    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    int current_hour = timeinfo.tm_hour;
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
        int32_t px[24], py[24];
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

        // Title (centered below chart) - dynamic based on precipitation type
        // Note: writeln y-coordinate is the text BASELINE, text extends upward ~20px
        char title[16];
        if (strcmp(precip_type, "snow") == 0) {
            strcpy(title, "Snow");
        } else if (strcmp(precip_type, "mixed") == 0) {
            strcpy(title, "Mixed");
        } else {
            strcpy(title, "Rain");
        }

        // Center the text horizontally
        // FiraSans ~12px per character for more accurate centering
        int32_t text_width = strlen(title) * 12;
        int32_t tx = x + (w - text_width) / 2;
        // Position baseline below chart with spacing for text height + gap
        int32_t ty = chart_bottom + 45;  // 45px below chart bottom
        writeln((GFXfont *)&FiraSans, title, &tx, &ty, fb);
    }
    // else: no precipitation - show empty graph with gridlines only (no label)

    // Time marker lines (drawn on top of all chart elements)
    // Major markers: midnight and noon (thick, dark)
    draw_time_marker(0,  current_hour, count, x, w, chart_y, chart_bottom, 0x40, 3, fb);
    draw_time_marker(12, current_hour, count, x, w, chart_y, chart_bottom, 0x40, 3, fb);
    // Minor markers: 6am and 6pm (thin, lighter)
    draw_time_marker(6,  current_hour, count, x, w, chart_y, chart_bottom, 0x60, 1, fb);
    draw_time_marker(18, current_hour, count, x, w, chart_y, chart_bottom, 0x60, 1, fb);
}
