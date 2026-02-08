#pragma once
#include "epd_driver.h"
#include <math.h>

enum WeatherIcon {
    SUNNY,
    CLOUDY,
    PARTLY_CLOUDY,
    RAINY,
    SNOWY
};

// Draw a sun centered at (cx, cy) with given radius
static void draw_sun(int32_t cx, int32_t cy, int32_t radius, uint8_t *fb) {
    // Filled circle for sun body
    epd_fill_circle(cx, cy, radius, 0x00, fb);

    // Radiating lines (8 rays)
    int32_t inner = radius + 6;
    int32_t outer = radius + 22;
    for (int i = 0; i < 8; i++) {
        float angle = i * M_PI / 4.0;
        int32_t x0 = cx + (int32_t)(inner * cos(angle));
        int32_t y0 = cy + (int32_t)(inner * sin(angle));
        int32_t x1 = cx + (int32_t)(outer * cos(angle));
        int32_t y1 = cy + (int32_t)(outer * sin(angle));
        epd_draw_line(x0, y0, x1, y1, 0x00, fb);
        // Draw parallel lines for thickness
        epd_draw_line(x0 + 1, y0, x1 + 1, y1, 0x00, fb);
        epd_draw_line(x0, y0 + 1, x1, y1 + 1, 0x00, fb);
    }
}

// Draw a cloud shape centered at (cx, cy) with given size scale
static void draw_cloud(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    // Cloud made of overlapping filled circles and a filled rect base
    int32_t r1 = size * 30 / 100;  // main top bump
    int32_t r2 = size * 22 / 100;  // left bump
    int32_t r3 = size * 25 / 100;  // right bump
    int32_t base_h = size * 15 / 100;

    // Base rectangle
    int32_t base_w = size * 70 / 100;
    int32_t base_x = cx - base_w / 2;
    int32_t base_y = cy;
    epd_fill_rect(base_x, base_y, base_w, base_h, 0x00, fb);

    // Top center bump
    epd_fill_circle(cx, cy - r1 / 3, r1, 0x00, fb);

    // Left bump
    epd_fill_circle(cx - size * 18 / 100, cy, r2, 0x00, fb);

    // Right bump
    epd_fill_circle(cx + size * 16 / 100, cy - r3 / 4, r3, 0x00, fb);
}

// Draw rain drops below a cloud
static void draw_rain(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    draw_cloud(cx, cy - size * 15 / 100, size, fb);

    // Angled rain lines below cloud
    int32_t drop_top = cy + size * 15 / 100;
    int32_t drop_len = size * 18 / 100;
    for (int i = 0; i < 5; i++) {
        int32_t dx = cx - size * 25 / 100 + i * size * 13 / 100;
        epd_draw_line(dx, drop_top + (i % 2) * 8, dx - 4, drop_top + drop_len + (i % 2) * 8, 0x00, fb);
        epd_draw_line(dx + 1, drop_top + (i % 2) * 8, dx - 3, drop_top + drop_len + (i % 2) * 8, 0x00, fb);
    }
}

// Draw snow dots below a cloud
static void draw_snow(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    draw_cloud(cx, cy - size * 15 / 100, size, fb);

    // Small circles (snowflakes) below cloud
    int32_t snow_y1 = cy + size * 18 / 100;
    int32_t snow_y2 = cy + size * 32 / 100;
    for (int i = 0; i < 4; i++) {
        int32_t dx = cx - size * 22 / 100 + i * size * 15 / 100;
        epd_fill_circle(dx, snow_y1, 3, 0x00, fb);
    }
    for (int i = 0; i < 3; i++) {
        int32_t dx = cx - size * 15 / 100 + i * size * 15 / 100;
        epd_fill_circle(dx, snow_y2, 3, 0x00, fb);
    }
}

// Draw partly cloudy: sun peeking from behind cloud
static void draw_partly_cloudy(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    // Sun in upper-left, partially behind cloud
    int32_t sun_r = size * 18 / 100;
    draw_sun(cx - size * 15 / 100, cy - size * 18 / 100, sun_r, fb);

    // White-out area where cloud will go (to cover sun rays)
    int32_t cloud_cx = cx + size * 8 / 100;
    int32_t cloud_cy = cy + size * 8 / 100;
    int32_t r1 = size * 25 / 100;
    int32_t r2 = size * 18 / 100;
    int32_t r3 = size * 20 / 100;
    int32_t base_w = size * 58 / 100;
    int32_t base_h = size * 12 / 100;

    // White fill behind cloud
    epd_fill_rect(cloud_cx - base_w / 2 - 2, cloud_cy - r1 / 2, base_w + 4, r1 + base_h + 4, 0xFF, fb);
    epd_fill_circle(cloud_cx, cloud_cy - r1 / 3, r1 + 3, 0xFF, fb);
    epd_fill_circle(cloud_cx - size * 14 / 100, cloud_cy, r2 + 3, 0xFF, fb);
    epd_fill_circle(cloud_cx + size * 13 / 100, cloud_cy - r3 / 4, r3 + 3, 0xFF, fb);

    // Then draw the cloud on top
    draw_cloud(cloud_cx, cloud_cy, size * 85 / 100, fb);
}

// Main dispatch function
static void draw_weather_icon(WeatherIcon icon, int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    switch (icon) {
        case SUNNY:        draw_sun(cx, cy, size * 30 / 100, fb); break;
        case CLOUDY:       draw_cloud(cx, cy, size, fb); break;
        case PARTLY_CLOUDY: draw_partly_cloudy(cx, cy, size, fb); break;
        case RAINY:        draw_rain(cx, cy, size, fb); break;
        case SNOWY:        draw_snow(cx, cy, size, fb); break;
    }
}
