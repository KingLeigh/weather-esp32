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
    epd_fill_circle(cx, cy, radius, 0x00, fb);

    int32_t inner = radius + 6;
    int32_t outer = radius + 22;
    for (int i = 0; i < 8; i++) {
        float angle = i * M_PI / 4.0;
        int32_t x0 = cx + (int32_t)(inner * cos(angle));
        int32_t y0 = cy + (int32_t)(inner * sin(angle));
        int32_t x1 = cx + (int32_t)(outer * cos(angle));
        int32_t y1 = cy + (int32_t)(outer * sin(angle));
        epd_draw_line(x0, y0, x1, y1, 0x00, fb);
        epd_draw_line(x0 + 1, y0, x1 + 1, y1, 0x00, fb);
        epd_draw_line(x0, y0 + 1, x1, y1 + 1, 0x00, fb);
    }
}

// Draw a puffy, cartoony cloud centered at (cx, cy) with given size scale
static void draw_cloud(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    // Many overlapping circles for a puffy look
    // Bottom row
    struct { int ox; int oy; int r; } puffs[] = {
        {-28,  8, 18}, {-10,  8, 20}, { 10,  8, 20}, { 28,  8, 18},  // bottom
        {-22, -8, 20}, {  0,-12, 24}, { 22, -6, 19},                  // middle
        { -4,-26, 20},                                                  // top
    };
    for (int i = 0; i < 8; i++) {
        int32_t rx = cx + puffs[i].ox * size / 100;
        int32_t ry = cy + puffs[i].oy * size / 100;
        int32_t rr = puffs[i].r * size / 100;
        epd_fill_circle(rx, ry, rr, 0x00, fb);
    }
}

// Draw rain drops below a cloud
static void draw_rain(int32_t cx, int32_t cy, int32_t size, uint8_t *fb) {
    draw_cloud(cx, cy - size * 15 / 100, size, fb);

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
    int32_t sun_r = size * 18 / 100;
    draw_sun(cx - size * 15 / 100, cy - size * 18 / 100, sun_r, fb);

    // White-out area where cloud will go
    int32_t cloud_cx = cx + size * 8 / 100;
    int32_t cloud_cy = cy + size * 8 / 100;
    int32_t cs = size * 85 / 100;

    // White puffs slightly larger to mask sun rays behind cloud
    struct { int ox; int oy; int r; } puffs[] = {
        {-28,  8, 18}, {-10,  8, 20}, { 10,  8, 20}, { 28,  8, 18},
        {-22, -8, 20}, {  0,-12, 24}, { 22, -6, 19},
        { -4,-26, 20},
    };
    for (int i = 0; i < 8; i++) {
        int32_t rx = cloud_cx + puffs[i].ox * cs / 100;
        int32_t ry = cloud_cy + puffs[i].oy * cs / 100;
        int32_t rr = puffs[i].r * cs / 100 + 3;
        epd_fill_circle(rx, ry, rr, 0xFF, fb);
    }

    draw_cloud(cloud_cx, cloud_cy, cs, fb);
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
