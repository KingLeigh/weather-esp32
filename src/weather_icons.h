#pragma once
#include "epd_driver.h"
#include "weather_icon_bitmaps.h"

#define ICON_SIZE 200  // All icons are 200x200

enum WeatherIcon {
    SUNNY,
    CLOUDY,
    PARTLY_CLOUDY,
    RAINY,
    SNOWY,
    THUNDERSTORM,
    FOG
};

// Small sun icon for UV index panel - hollow center with heavier rays
static void draw_sun_small(int32_t cx, int32_t cy, uint8_t *fb) {
    int32_t r = 14;
    uint8_t color = 0x50;  // soft gray

    // Draw hollow sun (outline only, not filled)
    epd_draw_circle(cx, cy, r, color, fb);
    epd_draw_circle(cx, cy, r - 1, color, fb);  // Thicken the outline

    // Draw heavier rays (8 rays, thicker than before)
    int32_t inner = r + 4;
    int32_t outer = r + 12;
    for (int i = 0; i < 8; i++) {
        float angle = i * M_PI / 4.0;
        int32_t x0 = cx + (int32_t)(inner * cos(angle));
        int32_t y0 = cy + (int32_t)(inner * sin(angle));
        int32_t x1 = cx + (int32_t)(outer * cos(angle));
        int32_t y1 = cy + (int32_t)(outer * sin(angle));

        // Draw thicker rays (3 parallel lines per ray)
        epd_draw_line(x0, y0, x1, y1, color, fb);
        epd_draw_line(x0 + 1, y0, x1 + 1, y1, color, fb);
        epd_draw_line(x0, y0 + 1, x1, y1 + 1, color, fb);
    }
}

static void draw_weather_icon(WeatherIcon icon, int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    // Icons are centered at cx, cy
    // Top-left corner for bitmap rendering
    int32_t x = cx - ICON_SIZE / 2;
    int32_t y = cy - ICON_SIZE / 2;

    // Create Rect_t for the icon area
    Rect_t area = {
        .x = x,
        .y = y,
        .width = ICON_SIZE,
        .height = ICON_SIZE
    };

    const uint8_t* bitmap = NULL;

    switch (icon) {
        case SUNNY:
            bitmap = icon_sun_200;
            break;
        case CLOUDY:
            bitmap = icon_cloud_200;
            break;
        case PARTLY_CLOUDY:
            bitmap = icon_partly_200;
            break;
        case RAINY:
            bitmap = icon_rainy_200;
            break;
        case SNOWY:
            bitmap = icon_snowflake_200;
            break;
        case THUNDERSTORM:
            bitmap = icon_lighting_200;
            break;
        case FOG:
            bitmap = icon_fog_200;
            break;
    }

    if (bitmap) {
        // Copy bitmap data into the framebuffer
        // The bitmap is already in the correct 4-bit format
        for (int row = 0; row < ICON_SIZE; row++) {
            for (int col = 0; col < ICON_SIZE / 2; col++) {
                int fb_x = x + col * 2;
                int fb_y = y + row;
                if (fb_x >= 0 && fb_x < EPD_WIDTH - 1 && fb_y >= 0 && fb_y < EPD_HEIGHT) {
                    int fb_index = (fb_y * EPD_WIDTH + fb_x) / 2;
                    int bitmap_index = (row * ICON_SIZE + col * 2) / 2;
                    fb[fb_index] = bitmap[bitmap_index];
                }
            }
        }
    }
}
