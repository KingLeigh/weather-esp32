#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM (-DBOARD_HAS_PSRAM)"
#endif

#include <Arduino.h>
#include <esp_adc_cal.h>
#include <time.h>
#include "epd_driver.h"
#include "utilities.h"
#include "firasans.h"
#include "fonts/font_large.h"
#include "fonts/font_medium.h"
#include "weather_icons.h"
#include "precip_chart.h"
#include "weather_fetch.h"
#include "moon_phase_bitmaps.h"

uint8_t *framebuffer = NULL;
uint32_t vref = 1100;  // ADC reference voltage

#define UPDATE_INTERVAL_SECONDS 300  // Update every 5 minutes

// Track previous weather data to detect changes
WeatherData prev_weather;
int prev_battery_percent = -1;
int prev_age_minutes = -1;  // Track previous age for change detection

// Parse ISO timestamp to epoch time (e.g., "2025-01-15T14:30:00" -> epoch seconds)
static time_t parse_timestamp_to_epoch(const char* timestamp) {
    if (!timestamp || strlen(timestamp) == 0) return 0;

    struct tm timeinfo = {0};
    int year, month, day, hour, minute, second;

    // Parse ISO 8601 format: YYYY-MM-DDTHH:MM:SS
    if (sscanf(timestamp, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        timeinfo.tm_year = year - 1900;  // tm_year is years since 1900
        timeinfo.tm_mon = month - 1;     // tm_mon is 0-11
        timeinfo.tm_mday = day;
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;
        timeinfo.tm_isdst = -1;          // Let mktime determine DST

        return mktime(&timeinfo);
    }

    return 0;  // Failed to parse
}

// Calculate data age in minutes (returns -1 if invalid)
static int get_data_age_minutes(const char* timestamp) {
    if (!timestamp || strlen(timestamp) == 0) {
        return -1;
    }

    // Get current time
    time_t now;
    time(&now);

    // Parse API timestamp
    time_t data_time = parse_timestamp_to_epoch(timestamp);

    if (data_time == 0 || now == 0) {
        return -1;  // Time not synced yet or parse failed
    }

    // Calculate age in seconds
    int age_seconds = (int)difftime(now, data_time);

    if (age_seconds < 0) {
        return 0;  // Clock skew - data timestamp is in the future
    }

    return age_seconds / 60;  // Return age in minutes
}

// Format age as string (e.g., "35m" or "1h 23m") - only if > 30 minutes
static void format_data_age(int age_minutes, char* output, size_t output_size) {
    if (age_minutes < 0) {
        // Invalid/unknown age - don't show anything
        output[0] = '\0';
        return;
    }

    if (age_minutes <= 30) {
        // Fresh data - don't show age
        output[0] = '\0';
        return;
    }

    // Stale data (> 30 minutes) - show age
    int age_hours = age_minutes / 60;
    int remaining_minutes = age_minutes % 60;

    if (age_hours > 0) {
        snprintf(output, output_size, "%dh %dm", age_hours, remaining_minutes);
    } else {
        snprintf(output, output_size, "%dm", age_minutes);
    }
}

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

// Map moon phase string to bitmap
static const uint8_t* get_moon_phase_bitmap(const char* phase) {
    if (strcmp(phase, "New Moon") == 0) {
        return moon_1_new_100;
    } else if (strcmp(phase, "Waxing Crescent") == 0) {
        return moon_2_crescent_100;
    } else if (strcmp(phase, "First Quarter") == 0) {
        return moon_3_quarter_100;
    } else if (strcmp(phase, "Waxing Gibbous") == 0) {
        return moon_4_gibbous_100;
    } else if (strcmp(phase, "Full Moon") == 0) {
        return moon_5_full_100;
    } else if (strcmp(phase, "Waning Gibbous") == 0) {
        return moon_6_gibbous_100;
    } else if (strcmp(phase, "Last Quarter") == 0 || strcmp(phase, "Third Quarter") == 0) {
        return moon_7_quarter_100;
    } else if (strcmp(phase, "Waning Crescent") == 0) {
        return moon_8_crescent_100;
    }
    // Default to full moon if unknown phase
    return moon_5_full_100;
}

// Draw a moon phase icon (100x100 bitmap)
static void draw_moon_icon(const uint8_t* bitmap, int32_t cx, int32_t cy, uint8_t *fb) {
    const int32_t size = 100;  // Moon icons are 100x100
    int32_t x = cx - size / 2;  // Top-left corner
    int32_t y = cy - size / 2;

    // Copy bitmap data into framebuffer (same pattern as weather icons)
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size / 2; col++) {
            int fb_x = x + col * 2;
            int fb_y = y + row;
            if (fb_x >= 0 && fb_x < EPD_WIDTH - 1 && fb_y >= 0 && fb_y < EPD_HEIGHT) {
                int fb_index = (fb_y * EPD_WIDTH + fb_x) / 2;
                int bitmap_index = (row * size + col * 2) / 2;
                fb[fb_index] = bitmap[bitmap_index];
            }
        }
    }
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
                           int* precip_pct, const char* precip_type, int uv_current, int uv_high,
                           const char* moon_phase, const char* sunrise, const char* sunset,
                           const char* age_str, int battery_percent) {
    // Clear framebuffer
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    // === TEXT BLOCK: Current temp, UV, High/Low ===
    // Change these two values to move the entire block
    int32_t base_x = 330, base_y = 130;

    // --- Current temperature (large font) ---
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d\xC2\xB0", current_temp);
    int32_t cx = base_x, cy = base_y;
    writeln((GFXfont *)&FiraSansLarge, temp_str, &cx, &cy, framebuffer);

    // --- UV Index (on same line as temperature) ---
    // Sun icon + current (large) + max (medium): ☀2 3
    draw_sun_small(cx + 70, cy - 30, framebuffer);  // Sun icon (centered with large text)

    // Current UV in large font
    char uv_current_str[4];
    snprintf(uv_current_str, sizeof(uv_current_str), "%d", uv_current);
    int32_t uvcx = cx + 105, uvcy = cy;
    writeln((GFXfont *)&FiraSansLarge, uv_current_str, &uvcx, &uvcy, framebuffer);

    // Max UV in medium font (positioned after current)
    char uv_max_str[4];
    snprintf(uv_max_str, sizeof(uv_max_str), "%d", uv_high);
    int32_t uvmx = uvcx + 10, uvmy = cy;  // Position after current UV
    writeln((GFXfont *)&FiraSansMedium, uv_max_str, &uvmx, &uvmy, framebuffer);

    // --- High / Low temps (medium font, below current temp) ---
    char hi_str[16], lo_str[16];
    snprintf(hi_str, sizeof(hi_str), "H: %d\xC2\xB0", high_temp);
    snprintf(lo_str, sizeof(lo_str), "L: %d\xC2\xB0", low_temp);

    int32_t hx = base_x, hy = base_y + 85;  // 85 pixels below current temp baseline
    writeln((GFXfont *)&FiraSansMedium, hi_str, &hx, &hy, framebuffer);

    int32_t lx = hx + 30, ly = base_y + 85;
    writeln((GFXfont *)&FiraSansMedium, lo_str, &lx, &ly, framebuffer);

    // --- Weather icon (top-left) ---
    draw_weather_icon(icon, 150, 130, 200, framebuffer);

    // --- Moon phase icon (top-right) ---
    const uint8_t* moon_bitmap = get_moon_phase_bitmap(moon_phase);
    draw_moon_icon(moon_bitmap, 820, 130, framebuffer);

    // --- Sunrise/Sunset times (lower right) ---
    int32_t sun_x = 700;
    int32_t sunrise_y = 380;
    int32_t sunset_y = 428;

    // Sunrise icon (sun on horizon) - centered above text
    draw_sunrise_icon(sun_x + 85, sunrise_y - 48, framebuffer);

    int32_t srx = sun_x, sry = sunrise_y;
    writeln((GFXfont *)&FiraSans, sunrise, &srx, &sry, framebuffer);

    int32_t ssx = sun_x, ssy = sunset_y;
    writeln((GFXfont *)&FiraSans, sunset, &ssx, &ssy, framebuffer);

    /* CARD-BASED LAYOUT - didn't work out
    // --- Temperature Card (left) ---
    int32_t temp_card_x = 40, temp_card_y = 30;
    int32_t temp_card_w = 400, temp_card_h = 200;

    // Card border (light gray)
    epd_draw_rect(temp_card_x, temp_card_y, temp_card_w, temp_card_h, 0xB0, framebuffer);

    // Current temp (large font, centered vertically)
    char temp_str[8];
    snprintf(temp_str, sizeof(temp_str), "%d\xC2\xB0", current_temp);
    int32_t ctx = temp_card_x + 140, cty = temp_card_y + 95;
    writeln((GFXfont *)&FiraSansLarge, temp_str, &ctx, &cty, framebuffer);

    // H/L temps (medium font, centered at bottom)
    char hi_str[16], lo_str[16];
    snprintf(hi_str, sizeof(hi_str), "H: %d\xC2\xB0", high_temp);
    snprintf(lo_str, sizeof(lo_str), "L: %d\xC2\xB0", low_temp);

    int32_t hx = temp_card_x + 60, hy = temp_card_y + 180;
    writeln((GFXfont *)&FiraSansMedium, hi_str, &hx, &hy, framebuffer);

    int32_t lx = hx + 30, ly = temp_card_y + 180;
    writeln((GFXfont *)&FiraSansMedium, lo_str, &lx, &ly, framebuffer);

    // --- UV Card (right) ---
    int32_t uv_card_x = 480, uv_card_y = 30;
    int32_t uv_card_w = 300, uv_card_h = 200;

    // Card border (light gray)
    epd_draw_rect(uv_card_x, uv_card_y, uv_card_w, uv_card_h, 0xB0, framebuffer);

    // Sun icon + current UV (large font, centered vertically)
    draw_sun_small(uv_card_x + 95, uv_card_y + 70, framebuffer);
    char uv_current_str[8];
    snprintf(uv_current_str, sizeof(uv_current_str), "%d", uv_current);
    int32_t uvcx = uv_card_x + 130, uvcy = uv_card_y + 85;
    writeln((GFXfont *)&FiraSansLarge, uv_current_str, &uvcx, &uvcy, framebuffer);

    // Max UV (medium font, at bottom)
    char uv_max_str[16];
    snprintf(uv_max_str, sizeof(uv_max_str), "Max: %d", uv_high);
    int32_t uvmx = uv_card_x + 95, uvmy = uv_card_y + 180;
    writeln((GFXfont *)&FiraSansMedium, uv_max_str, &uvmx, &uvmy, framebuffer);

    // --- Weather icon (top-right) ---
    draw_weather_icon(icon, 860, 130, 200, framebuffer);
    END CARD-BASED LAYOUT */

    /* OLD UV INDEX (beside H/L temps) - kept for potential reversion
    // --- UV Index (beside H/L temps on same line) ---
    draw_sun_small(lx + 80, 197, framebuffer);
    char uv_str[16];
    snprintf(uv_str, sizeof(uv_str), " %d / %d", uv_current, uv_high);
    int32_t uvx = lx + 107, uvy = 212;  // Positioned after L: temp
    writeln((GFXfont *)&FiraSans, uv_str, &uvx, &uvy, framebuffer);
    END OLD UV INDEX */

    // --- Divider line ---
    // epd_draw_hline(40, 265, 880, 0x80, framebuffer);  // Temporarily disabled

    // --- Precipitation chart (doubled width, 24 hours) ---
    draw_precip_chart(40, 270, 560, 210, precip_pct, PRECIP_HOURS, precip_type, framebuffer);

    /* OLD UV INDEX POSITION (lower-right section) - kept for potential reversion
    // --- UV Index (right half of lower section) ---
    // Simpler version: sun icon + "current / max" text
    draw_sun_small(570, 350, framebuffer);

    char uv_str[16];
    snprintf(uv_str, sizeof(uv_str), " %d / %d", uv_current, uv_high);
    int32_t uvx = 602, uvy = 365;
    writeln((GFXfont *)&FiraSans, uv_str, &uvx, &uvy, framebuffer);
    END OLD UV INDEX POSITION */

    /* OLD UV INDEX DISPLAY (with bar) - kept for potential reversion
    draw_sun_small(570, 325, framebuffer);
    int32_t uvlx = 608, uvly = 340;  // Positioned to center with sun icon with spacing
    writeln((GFXfont *)&FiraSans, "UV Index", &uvlx, &uvly, framebuffer);

    // Horizontal UV meter (same width as rain graph)
    int32_t meter_x = 560;
    int32_t meter_y = 375;
    int32_t meter_w = 280;  // Match rain graph width
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
    int32_t fill_w_hi = (uv_high > 0) ? (meter_w * uv_high / uv_max) : 0;
    if (uv_high > uv_current) {
        epd_fill_rect(meter_x + fill_w_now + 1, meter_y + 1, fill_w_hi - fill_w_now - 1, meter_h - 2, 0x80, framebuffer);
    }

    // Numbers positioned at their points on the meter (below)
    char uv_now_str[4], uv_hi_str[4];
    snprintf(uv_now_str, sizeof(uv_now_str), "%d", uv_current);
    snprintf(uv_hi_str, sizeof(uv_hi_str), "%d", uv_high);

    // Always show current UV (even if 0) at the left edge if no fill
    int32_t nx = uv_current > 0 ? (meter_x + fill_w_now - 8) : (meter_x + 1);
    int32_t ny = meter_y + meter_h + 40;
    writeln((GFXfont *)&FiraSans, uv_now_str, &nx, &ny, framebuffer);

    // Show high UV at its position on the meter
    if (uv_high > 0 && uv_high != uv_current) {
        int32_t hx = meter_x + fill_w_hi - 8;
        int32_t hy = meter_y + meter_h + 40;
        writeln((GFXfont *)&FiraSans, uv_hi_str, &hx, &hy, framebuffer);
    }
    END OLD UV INDEX DISPLAY */

    // --- Battery icon and data age (lower-right corner) ---
    int32_t battery_x = EPD_WIDTH - 55;
    int32_t battery_y = EPD_HEIGHT - 35;

    // Show battery icon
    draw_battery_icon(battery_x, battery_y, battery_percent, framebuffer);

    // Age to the left of battery (only if data is stale > 30 minutes)
    if (strlen(age_str) > 0) {
        int32_t age_x = battery_x - 80;  // Position age to the left of battery
        int32_t age_y = EPD_HEIGHT - 15;
        writeln((GFXfont *)&FiraSans, age_str, &age_x, &age_y, framebuffer);
    }

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

    // Check moon phase
    if (strcmp(old_data->moon_phase, new_data->moon_phase) != 0) return true;

    // Check precipitation array and type
    if (strcmp(old_data->precip_type, new_data->precip_type) != 0) return true;
    for (int i = 0; i < PRECIP_HOURS; i++) {
        if (old_data->precipitation[i] != new_data->precipitation[i]) return true;
    }

    // Check sunrise/sunset
    if (strcmp(old_data->sunrise, new_data->sunrise) != 0) return true;
    if (strcmp(old_data->sunset, new_data->sunset) != 0) return true;

    // Check battery (allow 10% tolerance to avoid flashing on minor voltage fluctuations)
    if (abs(old_battery - new_battery) > 10) return true;

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
        } else {
            Serial.println("Failed to fetch weather data");
        }
        disconnectWiFi();  // Save power
    } else {
        Serial.println("WiFi connection failed");
    }

    // Use fetched data, or show zeros if fetch failed (clear error indication)
    int current_temp = weather.valid ? weather.temp_current : 0;
    int high_temp = weather.valid ? weather.temp_high : 0;
    int low_temp = weather.valid ? weather.temp_low : 0;
    WeatherIcon icon = weather.valid ? weather.weather : CLOUDY;  // Cloudy = error indicator
    int precip_zero[PRECIP_HOURS] = {0};
    int* precip_pct = weather.valid ? weather.precipitation : precip_zero;
    const char* precip_type = weather.valid ? weather.precip_type : "rain";
    int uv_current = weather.valid ? weather.uv_current : 0;
    int uv_high = weather.valid ? weather.uv_high : 0;
    const char* moon_phase = weather.valid ? weather.moon_phase : "Full Moon";
    const char* sunrise = weather.valid ? weather.sunrise : "6:00 AM";
    const char* sunset = weather.valid ? weather.sunset : "6:00 PM";

    // Read battery and calculate data age
    int battery_percent = read_battery_percent();
    const char* timestamp = weather.valid ? weather.updated : prev_weather.updated;
    int age_minutes = get_data_age_minutes(timestamp);
    char age_str[16];
    format_data_age(age_minutes, age_str, sizeof(age_str));

    render_display(current_temp, high_temp, low_temp, icon, precip_pct, precip_type,
                   uv_current, uv_high, moon_phase, sunrise, sunset, age_str, battery_percent);

    Serial.println("Weather display updated");
    Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);

    // Save current weather data for change detection
    prev_weather = weather;
    prev_battery_percent = battery_percent;
    prev_age_minutes = age_minutes;
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
        } else {
            Serial.println("Failed to fetch weather data");
        }
        disconnectWiFi();
    } else {
        Serial.println("WiFi connection failed");
    }

    // Use fetched data if valid, otherwise keep showing previous data
    int current_temp = weather.valid ? weather.temp_current : prev_weather.temp_current;
    int high_temp = weather.valid ? weather.temp_high : prev_weather.temp_high;
    int low_temp = weather.valid ? weather.temp_low : prev_weather.temp_low;
    WeatherIcon icon = weather.valid ? weather.weather : prev_weather.weather;
    int* precip_pct = weather.valid ? weather.precipitation : prev_weather.precipitation;
    const char* precip_type = weather.valid ? weather.precip_type : prev_weather.precip_type;
    int uv_current = weather.valid ? weather.uv_current : prev_weather.uv_current;
    int uv_high = weather.valid ? weather.uv_high : prev_weather.uv_high;
    const char* moon_phase = weather.valid ? weather.moon_phase : prev_weather.moon_phase;
    const char* sunrise = weather.valid ? weather.sunrise : prev_weather.sunrise;
    const char* sunset = weather.valid ? weather.sunset : prev_weather.sunset;

    // Read battery and calculate data age
    const char* timestamp = weather.valid ? weather.updated : prev_weather.updated;
    int battery_percent = read_battery_percent();
    int age_minutes = get_data_age_minutes(timestamp);
    char age_str[16];
    format_data_age(age_minutes, age_str, sizeof(age_str));

    // Check if we should update the display
    bool data_changed = weather_data_changed(&prev_weather, &weather, prev_battery_percent, battery_percent);
    bool prev_showing_age = (prev_age_minutes > 30);
    bool now_showing_age = (age_minutes > 30);
    bool age_display_changed = (prev_showing_age != now_showing_age);
    bool should_update = data_changed || age_display_changed;

    if (!should_update) {
        Serial.println("No changes detected - skipping display update");
        Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);
        // Still save state even if not updating display
        prev_weather = weather;
        prev_battery_percent = battery_percent;
        prev_age_minutes = age_minutes;
        return;
    }

    Serial.println("Data changed - updating display");

    // Render display
    render_display(current_temp, high_temp, low_temp, icon, precip_pct, precip_type,
                   uv_current, uv_high, moon_phase, sunrise, sunset, age_str, battery_percent);

    Serial.println("Weather display updated");
    Serial.printf("Next update in %d seconds...\n\n", UPDATE_INTERVAL_SECONDS);

    // Save current weather data for next comparison
    prev_weather = weather;
    prev_battery_percent = battery_percent;
    prev_age_minutes = age_minutes;
}
