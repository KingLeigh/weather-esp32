#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM (-DBOARD_HAS_PSRAM)"
#endif

#include <Arduino.h>
#include <esp_sleep.h>
#include "epd_driver.h"
#include "firasans.h"
#include "fonts/font_large.h"
#include "fonts/font_medium.h"
#include "weather_icons.h"
#include "precip_chart.h"

uint8_t *framebuffer = NULL;

// --- Hardcoded test data ---
int current_temp = 72;
int high_temp = 78;
int low_temp = 65;
WeatherIcon icon = SUNNY;
int precip_pct[] = {0, 0, 10, 20, 45, 60, 40, 20, 10, 5, 0, 0};

#define SLEEP_MINUTES 1

void setup()
{
    Serial.begin(115200);
    Serial.println("Weather Display");

    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("Failed to allocate framebuffer!");
        while (1);
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    epd_init();

    // --- Current temperature (large font, top-left) ---
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d\xC2\xB0", current_temp); // UTF-8 degree sign
    int32_t cx = 50, cy = 130;
    writeln((GFXfont *)&FiraSansLarge, temp_str, &cx, &cy, framebuffer);

    // --- Weather icon (top-right) ---
    draw_weather_icon(icon, 780, 110, 200, framebuffer);

    // --- High / Low temps (medium font, below current temp) ---
    char hi_str[16], lo_str[16];
    snprintf(hi_str, sizeof(hi_str), "H: %d\xC2\xB0", high_temp);
    snprintf(lo_str, sizeof(lo_str), "L: %d\xC2\xB0", low_temp);

    int32_t hx = 50, hy = 215;
    writeln((GFXfont *)&FiraSansMedium, hi_str, &hx, &hy, framebuffer);

    int32_t lx = hx + 30, ly = 215;
    writeln((GFXfont *)&FiraSansMedium, lo_str, &lx, &ly, framebuffer);

    // --- Divider line ---
    epd_draw_hline(40, 265, 880, 0x80, framebuffer);

    // --- Precipitation chart ---
    draw_precip_chart(40, 280, 880, 200, precip_pct, 12, framebuffer);

    // --- Last updated (small font, bottom) ---
    // Random seconds value so each wake cycle shows a visible change
    int rand_sec = esp_random() % 60;
    char updated_str[40];
    snprintf(updated_str, sizeof(updated_str), "Last updated: 2:30:%02d PM", rand_sec);
    int32_t ux = 40, uy = 520;
    writeln((GFXfont *)&FiraSans, updated_str, &ux, &uy, framebuffer);

    // Push to display
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    Serial.println("Weather display updated");

    // Free framebuffer before sleep
    free(framebuffer);
    framebuffer = NULL;

    // Enter deep sleep
    Serial.printf("Sleeping for %d minute(s)...\n", SLEEP_MINUTES);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
    esp_deep_sleep_start();
}

void loop()
{
    // Never reached — deep sleep restarts from setup()
}
