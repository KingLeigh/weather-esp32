#pragma once
#include <time.h>
#include "epd_driver.h"
#include "firasans.h"

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

    // Draw dotted vertical line at midnight (if within 24hr window)
    // Draw on top of all other elements so it's always visible
    int hours_until_midnight = (24 - current_hour) % 24;
    if (hours_until_midnight > 0 && hours_until_midnight < count) {
        int32_t midnight_x = x + (int32_t)((int64_t)hours_until_midnight * w / (count - 1));
        // Draw thicker, darker dotted line (3 pixels wide)
        for (int32_t dy = chart_y; dy < chart_bottom; dy += 8) {
            epd_draw_vline(midnight_x - 1, dy, 5, 0x40, fb);  // Left line (darker gray)
            epd_draw_vline(midnight_x, dy, 5, 0x40, fb);      // Center line
            epd_draw_vline(midnight_x + 1, dy, 5, 0x40, fb);  // Right line
        }
    }

    // Draw dotted vertical line at noon (if within 24hr window)
    int hours_until_noon = (12 - current_hour + 24) % 24;
    if (hours_until_noon > 0 && hours_until_noon < count) {
        int32_t noon_x = x + (int32_t)((int64_t)hours_until_noon * w / (count - 1));
        // Draw thicker, darker dotted line (3 pixels wide)
        for (int32_t dy = chart_y; dy < chart_bottom; dy += 8) {
            epd_draw_vline(noon_x - 1, dy, 5, 0x40, fb);  // Left line (darker gray)
            epd_draw_vline(noon_x, dy, 5, 0x40, fb);      // Center line
            epd_draw_vline(noon_x + 1, dy, 5, 0x40, fb);  // Right line
        }
    }

    // Draw lighter dotted vertical lines at 6am and 6pm (if within 24hr window)
    int hours_until_6am = (6 - current_hour + 24) % 24;
    if (hours_until_6am > 0 && hours_until_6am < count) {
        int32_t am6_x = x + (int32_t)((int64_t)hours_until_6am * w / (count - 1));
        // Draw lighter, thinner dotted line (1 pixel wide)
        for (int32_t dy = chart_y; dy < chart_bottom; dy += 8) {
            epd_draw_vline(am6_x, dy, 5, 0x60, fb);  // Medium-light gray
        }
    }

    int hours_until_6pm = (18 - current_hour + 24) % 24;
    if (hours_until_6pm > 0 && hours_until_6pm < count) {
        int32_t pm6_x = x + (int32_t)((int64_t)hours_until_6pm * w / (count - 1));
        // Draw lighter, thinner dotted line (1 pixel wide)
        for (int32_t dy = chart_y; dy < chart_bottom; dy += 8) {
            epd_draw_vline(pm6_x, dy, 5, 0x60, fb);  // Medium-light gray
        }
    }
}
