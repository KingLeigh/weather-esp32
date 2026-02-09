#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM (-DBOARD_HAS_PSRAM)"
#endif

#include <Arduino.h>
#include "epd_driver.h"
#include "utilities.h"
#include "firasans.h"
#include "weather_icons.h"

uint8_t *framebuffer = NULL;

void setup() {
    Serial.begin(115200);
    Serial.println("Weather Icons Preview");

    // Allocate framebuffer
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("Failed to allocate framebuffer!");
        while (1);
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    epd_init();

    // Layout: 3 rows, varying columns
    // Row 1: SUNNY, PARTLY_CLOUDY, CLOUDY (3 icons)
    // Row 2: RAINY, SNOWY, THUNDERSTORM (3 icons)
    // Row 3: FOG (1 icon, centered)

    const int icon_size = 180;
    const int spacing_x = 300;
    const int spacing_y = 240;
    const int start_x = 160;
    const int start_y = 100;

    // Row 1
    draw_weather_icon(SUNNY, start_x, start_y, icon_size, framebuffer);
    int32_t tx = start_x - 40, ty = start_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "SUNNY", &tx, &ty, framebuffer);

    draw_weather_icon(PARTLY_CLOUDY, start_x + spacing_x, start_y, icon_size, framebuffer);
    tx = start_x + spacing_x - 80; ty = start_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "PARTLY CLOUDY", &tx, &ty, framebuffer);

    draw_weather_icon(CLOUDY, start_x + spacing_x * 2, start_y, icon_size, framebuffer);
    tx = start_x + spacing_x * 2 - 50; ty = start_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "CLOUDY", &tx, &ty, framebuffer);

    // Row 2
    draw_weather_icon(RAINY, start_x, start_y + spacing_y, icon_size, framebuffer);
    tx = start_x - 40; ty = start_y + spacing_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "RAINY", &tx, &ty, framebuffer);

    draw_weather_icon(SNOWY, start_x + spacing_x, start_y + spacing_y, icon_size, framebuffer);
    tx = start_x + spacing_x - 40; ty = start_y + spacing_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "SNOWY", &tx, &ty, framebuffer);

    draw_weather_icon(THUNDERSTORM, start_x + spacing_x * 2, start_y + spacing_y, icon_size, framebuffer);
    tx = start_x + spacing_x * 2 - 85; ty = start_y + spacing_y + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "THUNDERSTORM", &tx, &ty, framebuffer);

    // Row 3 - FOG centered
    draw_weather_icon(FOG, EPD_WIDTH / 2, start_y + spacing_y * 2, icon_size, framebuffer);
    tx = EPD_WIDTH / 2 - 25; ty = start_y + spacing_y * 2 + icon_size/2 + 60;
    writeln((GFXfont *)&FiraSans, "FOG", &tx, &ty, framebuffer);

    // Push to display
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    Serial.println("Icons displayed! Take a photo.");
}

void loop() {
    delay(1000);
}
