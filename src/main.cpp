#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM (-DBOARD_HAS_PSRAM)"
#endif

#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_adc_cal.h>
#include "epd_driver.h"
#include "utilities.h"
#include "firasans.h"
#include "fonts/font_large.h"
#include "fonts/font_medium.h"
#include "weather_icons.h"
#include "precip_chart.h"

uint8_t *framebuffer = NULL;
uint32_t vref = 1100;  // ADC reference voltage

#define SLEEP_MINUTES 1

// Draw small battery icon with fill level
static void draw_battery_icon(int32_t x, int32_t y, int percent, uint8_t *fb) {
    int32_t w = 20, h = 10, tip_w = 2;

    // Battery body outline
    epd_draw_rect(x, y, w, h, 0xA0, fb);
    // Battery tip
    epd_fill_rect(x + w, y + 3, tip_w, h - 6, 0xA0, fb);

    // Fill level
    int32_t fill_w = (w - 2) * percent / 100;
    if (fill_w > 0) {
        epd_fill_rect(x + 1, y + 1, fill_w, h - 2, 0x50, fb);
    }
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Weather Display");

    // Calibrate ADC for battery reading
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
        ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12,
        1100, &adc_chars
    );
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }

    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("Failed to allocate framebuffer!");
        while (1);
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    epd_init();

    // --- Random test data ---
    int current_temp = 50 + (int)(esp_random() % 50);       // 50-99
    int high_temp = current_temp + (int)(esp_random() % 15); // current + 0-14
    int low_temp = current_temp - (int)(esp_random() % 15);  // current - 0-14
    WeatherIcon icon = (WeatherIcon)(esp_random() % 5);
    int precip_pct[12];
    for (int i = 0; i < 12; i++) {
        precip_pct[i] = (int)(esp_random() % 101);          // 0-100
    }
    int uv_current = (int)(esp_random() % 12);               // 0-11
    int uv_high = uv_current + (int)(esp_random() % (12 - uv_current)); // current-11

    // Read battery voltage
    epd_poweron();
    delay(10);
    uint16_t v = analogRead(BATT_PIN);
    epd_poweroff();
    float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
    if (battery_voltage > 4.2) battery_voltage = 4.2;
    int battery_percent = (int)((battery_voltage - 3.0) / 1.2 * 100);
    if (battery_percent < 0) battery_percent = 0;
    if (battery_percent > 100) battery_percent = 100;

    // --- Current temperature (large font, top-left) ---
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d\xC2\xB0", current_temp);
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

    // --- Precipitation chart (half width) ---
    draw_precip_chart(40, 280, 440, 200, precip_pct, 12, framebuffer);

    // --- UV Index (right half of lower section) ---
    draw_sun_small(570, 310, framebuffer);
    int32_t uvlx = 600, uvly = 320;
    writeln((GFXfont *)&FiraSans, "UV Index", &uvlx, &uvly, framebuffer);

    char uv_now_str[4], uv_hi_str[4];
    snprintf(uv_now_str, sizeof(uv_now_str), "%d", uv_current);
    snprintf(uv_hi_str, sizeof(uv_hi_str), "%d", uv_high);

    int32_t unx = 540, uny = 375;
    writeln((GFXfont *)&FiraSans, "Now", &unx, &uny, framebuffer);
    int32_t unvx = 610, unvy = 375;
    writeln((GFXfont *)&FiraSansMedium, uv_now_str, &unvx, &unvy, framebuffer);

    int32_t uhx = 540, uhy = 435;
    writeln((GFXfont *)&FiraSans, "High", &uhx, &uhy, framebuffer);
    int32_t uhvx = 610, uhvy = 435;
    writeln((GFXfont *)&FiraSansMedium, uv_hi_str, &uhvx, &uhvy, framebuffer);

    // --- Timestamp with battery icon (lower-right corner, subtle gray) ---
    int rand_sec = esp_random() % 60;
    char updated_str[16];
    snprintf(updated_str, sizeof(updated_str), "2:30:%02d PM", rand_sec);
    int32_t ux = EPD_WIDTH - 160, uy = EPD_HEIGHT - 15;

    // Battery icon to the left of timestamp (centered vertically)
    draw_battery_icon(ux - 30, uy - 15, battery_percent, framebuffer);

    writeln((GFXfont *)&FiraSans, updated_str, &ux, &uy, framebuffer);

    // Push to display
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    Serial.println("Weather display updated");

#if 0 // Enable for deployment — deep sleep between refreshes
    free(framebuffer);
    framebuffer = NULL;
    Serial.printf("Sleeping for %d minute(s)...\n", SLEEP_MINUTES);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_MINUTES * 60 * 1000000ULL);
    esp_deep_sleep_start();
#endif
}

void loop()
{
    // Nothing to do — e-paper retains image without power
}
