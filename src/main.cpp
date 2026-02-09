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
#include "weather_fetch.h"

uint8_t *framebuffer = NULL;
uint32_t vref = 1100;  // ADC reference voltage

#define UPDATE_INTERVAL_SECONDS 60  // Update every 60 seconds

// Track previous weather data to detect changes
WeatherData prev_weather;
int prev_battery_percent = -1;
int consecutive_failures = 0;  // Track how many updates failed in a row

// Read battery percentage from ADC
static int read_battery_percent() {
    epd_poweron();
    delay(10);
    uint16_t v = analogRead(BATT_PIN);
    epd_poweroff();

    float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
    if (battery_voltage > 4.2) battery_voltage = 4.2;

    int battery_percent = (int)((battery_voltage - 3.0) / 1.2 * 100);
    if (battery_percent < 0) battery_percent = 0;
    if (battery_percent > 100) battery_percent = 100;

    return battery_percent;
}

// Draw battery icon with fill level
static void draw_battery_icon(int32_t x, int32_t y, int percent, uint8_t *fb) {
    int32_t w = 40, h = 20, tip_w = 4;

    // Battery body outline
    epd_draw_rect(x, y, w, h, 0xA0, fb);
    // Battery tip
    epd_fill_rect(x + w, y + 6, tip_w, h - 12, 0xA0, fb);

    // Fill level
    int32_t fill_w = (w - 2) * percent / 100;
    if (fill_w > 0) {
        epd_fill_rect(x + 1, y + 1, fill_w, h - 2, 0x50, fb);
    }
}

// Render all display elements to framebuffer
static void render_display(int current_temp, int high_temp, int low_temp, WeatherIcon icon,
                           int* precip_pct, int current_hour, int uv_current, int uv_high,
                           const char* timestamp, int battery_percent, int failures) {
    // Clear framebuffer
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

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

    // --- Precipitation chart (half width, moved down for more room) ---
    draw_precip_chart(40, 295, 440, 210, precip_pct, 12, current_hour, framebuffer);

    // --- UV Index (right half of lower section) ---
    draw_sun_small(570, 325, framebuffer);
    int32_t uvlx = 600, uvly = 335;
    writeln((GFXfont *)&FiraSans, "UV Index", &uvlx, &uvly, framebuffer);

    // Horizontal UV meter
    int32_t meter_x = 560;
    int32_t meter_y = 375;
    int32_t meter_w = 340;
    int32_t meter_h = 30;
    int32_t uv_max = 11;

    // Outline
    epd_draw_rect(meter_x, meter_y, meter_w, meter_h, 0xA0, framebuffer);

    // Black fill for current UV
    int32_t fill_w_now = 0;
    if (uv_current > 0) {
        fill_w_now = meter_w * uv_current / uv_max;
        epd_fill_rect(meter_x + 1, meter_y + 1, fill_w_now - 1, meter_h - 2, 0x00, framebuffer);
    }

    // Gray fill for high UV (from current to high)
    int32_t fill_w_hi = 0;
    if (uv_high > uv_current) {
        fill_w_hi = meter_w * uv_high / uv_max;
        epd_fill_rect(meter_x + fill_w_now + 1, meter_y + 1, fill_w_hi - fill_w_now - 1, meter_h - 2, 0x80, framebuffer);
    } else if (uv_high > 0) {
        fill_w_hi = meter_w * uv_high / uv_max;
    }

    // Numbers positioned at their points on the meter (below)
    char uv_now_str[4], uv_hi_str[4];
    snprintf(uv_now_str, sizeof(uv_now_str), "%d", uv_current);
    snprintf(uv_hi_str, sizeof(uv_hi_str), "%d", uv_high);

    // Always show current UV (even if 0) at the left edge if no fill
    int32_t nx = uv_current > 0 ? (meter_x + fill_w_now - 8) : (meter_x + 5);
    int32_t ny = meter_y + meter_h + 40;
    writeln((GFXfont *)&FiraSans, uv_now_str, &nx, &ny, framebuffer);

    // Show high UV at its position on the meter
    if (uv_high > 0 && uv_high != uv_current) {
        int32_t hx = meter_x + fill_w_hi - 8;
        int32_t hy = meter_y + meter_h + 40;
        writeln((GFXfont *)&FiraSans, uv_hi_str, &hx, &hy, framebuffer);
    }

    // --- Timestamp with battery icon (lower-right corner) ---
    char updated_str[16];
    if (strlen(timestamp) > 0) {
        // Parse ISO timestamp and extract time
        int hour, minute, second;
        if (sscanf(timestamp, "%*d-%*d-%*dT%d:%d:%d", &hour, &minute, &second) == 3) {
            snprintf(updated_str, sizeof(updated_str), "%02d:%02d:%02d", hour, minute, second);
        } else {
            strcpy(updated_str, "??:??:??");
        }
    } else {
        // No timestamp available (first boot and failed)
        strcpy(updated_str, "--:--:--");
    }
    int32_t ux = EPD_WIDTH - 170, uy = EPD_HEIGHT - 15;

    // Failure indicator to the left of battery icon (if any failures)
    if (failures > 0) {
        char failure_str[8];
        if (failures == 1) {
            strcpy(failure_str, "?");
        } else {
            snprintf(failure_str, sizeof(failure_str), "%d?", failures);
        }
        int32_t fx = ux - 100, fy = uy;
        writeln((GFXfont *)&FiraSans, failure_str, &fx, &fy, framebuffer);
    }

    // Battery icon to the left of timestamp (centered vertically)
    draw_battery_icon(ux - 55, uy - 20, battery_percent, framebuffer);

    writeln((GFXfont *)&FiraSans, updated_str, &ux, &uy, framebuffer);

    // Push to display
    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

// Check if weather data has changed (including timestamp)
static bool weather_data_changed(const WeatherData* old_data, const WeatherData* new_data, int old_battery, int new_battery) {
    // Always update if validity changed
    if (old_data->valid != new_data->valid) return true;

    // If both invalid, no change
    if (!old_data->valid && !new_data->valid) return false;

    // Check timestamp - this shows data freshness and prevents ghosting
    if (strcmp(old_data->updated, new_data->updated) != 0) return true;

    // Check temperature changes
    if (old_data->temp_current != new_data->temp_current) return true;
    if (old_data->temp_high != new_data->temp_high) return true;
    if (old_data->temp_low != new_data->temp_low) return true;

    // Check weather icon
    if (old_data->weather != new_data->weather) return true;

    // Check UV
    if (old_data->uv_current != new_data->uv_current) return true;
    if (old_data->uv_high != new_data->uv_high) return true;

    // Check precipitation array
    for (int i = 0; i < 12; i++) {
        if (old_data->precipitation[i] != new_data->precipitation[i]) return true;
    }

    // Check battery (allow 5% tolerance to avoid flashing on minor voltage fluctuations)
    if (abs(old_battery - new_battery) > 5) return true;

    // No significant changes
    return false;
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

    // --- Connect to WiFi and fetch weather data ---
    WeatherData weather;
    weather.valid = false;

    if (connectWiFi()) {
        if (fetchWeatherData(&weather)) {
            Serial.println("Weather data fetched successfully!");
            consecutive_failures = 0;
        } else {
            Serial.println("Failed to fetch weather data");
            consecutive_failures = 1;
        }
        disconnectWiFi();  // Save power
    } else {
        Serial.println("WiFi connection failed");
        consecutive_failures = 1;
    }

    // Use fetched data, or show zeros if fetch failed (clear error indication)
    int current_temp = weather.valid ? weather.temp_current : 0;
    int high_temp = weather.valid ? weather.temp_high : 0;
    int low_temp = weather.valid ? weather.temp_low : 0;
    WeatherIcon icon = weather.valid ? weather.weather : CLOUDY;  // Cloudy = error indicator
    int precip_zero[12] = {0,0,0,0,0,0,0,0,0,0,0,0};
    int* precip_pct = weather.valid ? weather.precipitation : precip_zero;
    int uv_current = weather.valid ? weather.uv_current : 0;
    int uv_high = weather.valid ? weather.uv_high : 0;

    // Parse current hour from timestamp for precipitation chart labels
    int current_hour = 0;  // Default to midnight if parsing fails
    if (weather.valid && strlen(weather.updated) > 0) {
        int hour, minute, second;
        if (sscanf(weather.updated, "%*d-%*d-%*dT%d:%d:%d", &hour, &minute, &second) == 3) {
            current_hour = hour;
        }
    }

    // Read battery and render display
    int battery_percent = read_battery_percent();
    const char* timestamp_for_display = weather.valid ? weather.updated : prev_weather.updated;
    render_display(current_temp, high_temp, low_temp, icon, precip_pct, current_hour,
                   uv_current, uv_high, timestamp_for_display, battery_percent, consecutive_failures);

    Serial.println("Weather display updated");
    Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);

    // Save current weather data for change detection
    prev_weather = weather;
    prev_battery_percent = battery_percent;
}

void loop()
{
    // Wait before next update
    delay(UPDATE_INTERVAL_SECONDS * 1000);

    Serial.println("\n=== Starting new weather update ===");

    // Fetch fresh weather data
    WeatherData weather;
    weather.valid = false;

    if (connectWiFi()) {
        if (fetchWeatherData(&weather)) {
            Serial.println("Weather data fetched successfully!");
            consecutive_failures = 0;  // Reset failure counter on success
        } else {
            Serial.println("Failed to fetch weather data");
            consecutive_failures++;
        }
        disconnectWiFi();
    } else {
        Serial.println("WiFi connection failed");
        consecutive_failures++;
    }

    // Use fetched data if valid, otherwise keep showing previous data
    int current_temp = weather.valid ? weather.temp_current : prev_weather.temp_current;
    int high_temp = weather.valid ? weather.temp_high : prev_weather.temp_high;
    int low_temp = weather.valid ? weather.temp_low : prev_weather.temp_low;
    WeatherIcon icon = weather.valid ? weather.weather : prev_weather.weather;
    int* precip_pct = weather.valid ? weather.precipitation : prev_weather.precipitation;
    int uv_current = weather.valid ? weather.uv_current : prev_weather.uv_current;
    int uv_high = weather.valid ? weather.uv_high : prev_weather.uv_high;

    // Parse current hour from timestamp for precipitation chart labels
    int current_hour = 0;  // Default to midnight if parsing fails
    const char* timestamp = weather.valid ? weather.updated : prev_weather.updated;
    if (strlen(timestamp) > 0) {
        int hour, minute, second;
        if (sscanf(timestamp, "%*d-%*d-%*dT%d:%d:%d", &hour, &minute, &second) == 3) {
            current_hour = hour;
        }
    }

    // Read battery and check for changes
    int battery_percent = read_battery_percent();
    bool data_changed = weather_data_changed(&prev_weather, &weather, prev_battery_percent, battery_percent);

    if (!data_changed) {
        Serial.println("No changes detected - skipping display update");
        Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);
        return;  // Skip display update
    }

    Serial.println("Data changed - updating display");

    // Render display
    const char* timestamp_for_display = weather.valid ? weather.updated : prev_weather.updated;
    render_display(current_temp, high_temp, low_temp, icon, precip_pct, current_hour,
                   uv_current, uv_high, timestamp_for_display, battery_percent, consecutive_failures);

    Serial.println("Weather display updated");
    Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);

    // Save current weather data for next comparison
    prev_weather = weather;
    prev_battery_percent = battery_percent;
}
