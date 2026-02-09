/**
 * Weather data fetching via WiFi/HTTP
 */

#pragma once

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "wifi_config.h"
#include "weather_icons.h"

// Weather data structure
struct WeatherData {
    int temp_current;
    int temp_high;
    int temp_low;
    WeatherIcon weather;
    int precipitation[12];
    int uv_current;
    int uv_high;
    char updated[32];
    bool valid;
};

// Connect to WiFi
bool connectWiFi() {
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    Serial.printf("Password length: %d characters\n", strlen(WIFI_PASSWORD));

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(500);
        Serial.print(".");
        attempts++;

        // Show status every 5 seconds
        if (attempts % 10 == 0) {
            Serial.printf("\nStatus: %d (", WiFi.status());
            switch (WiFi.status()) {
                case WL_IDLE_STATUS: Serial.print("IDLE"); break;
                case WL_NO_SSID_AVAIL: Serial.print("NO_SSID"); break;
                case WL_SCAN_COMPLETED: Serial.print("SCAN_DONE"); break;
                case WL_CONNECTED: Serial.print("CONNECTED"); break;
                case WL_CONNECT_FAILED: Serial.print("FAILED"); break;
                case WL_CONNECTION_LOST: Serial.print("LOST"); break;
                case WL_DISCONNECTED: Serial.print("DISCONNECTED"); break;
                default: Serial.print("UNKNOWN"); break;
            }
            Serial.print(")\n");
        }
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("Signal: %d dBm\n", WiFi.RSSI());
        return true;
    } else {
        Serial.printf("WiFi connection failed! Final status: %d\n", WiFi.status());
        if (WiFi.status() == WL_NO_SSID_AVAIL) {
            Serial.println("ERROR: Network not found! Check SSID spelling.");
        } else if (WiFi.status() == WL_CONNECT_FAILED) {
            Serial.println("ERROR: Connection failed! Check password.");
        }
        return false;
    }
}

// Map weather string to WeatherIcon enum
WeatherIcon parseWeatherIcon(const char* weather_str) {
    if (strcmp(weather_str, "sunny") == 0) return SUNNY;
    if (strcmp(weather_str, "cloudy") == 0) return CLOUDY;
    if (strcmp(weather_str, "partly_cloudy") == 0) return PARTLY_CLOUDY;
    if (strcmp(weather_str, "rainy") == 0) return RAINY;
    if (strcmp(weather_str, "snowy") == 0) return SNOWY;
    return PARTLY_CLOUDY;  // default
}

// Fetch weather data from API
bool fetchWeatherData(WeatherData* data) {
    if (!data) return false;

    data->valid = false;

    HTTPClient http;
    http.begin(WEATHER_API_URL);
    http.setTimeout(10000);  // 10 second timeout

    Serial.printf("Fetching weather from: %s\n", WEATHER_API_URL);

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("Received %d bytes\n", payload.length());

    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        Serial.printf("JSON parse error: %s\n", error.c_str());
        return false;
    }

    // Extract data
    data->temp_current = doc["temperature"]["current"] | 0;
    data->temp_high = doc["temperature"]["high"] | 0;
    data->temp_low = doc["temperature"]["low"] | 0;

    const char* weather_str = doc["weather"] | "partly_cloudy";
    data->weather = parseWeatherIcon(weather_str);

    // Precipitation array
    JsonArray precip_array = doc["precipitation"];
    for (int i = 0; i < 12; i++) {
        data->precipitation[i] = (i < precip_array.size()) ? precip_array[i].as<int>() : 0;
    }

    data->uv_current = doc["uv"]["current"] | 0;
    data->uv_high = doc["uv"]["high"] | 0;

    const char* updated_str = doc["updated"] | "";
    strncpy(data->updated, updated_str, sizeof(data->updated) - 1);
    data->updated[sizeof(data->updated) - 1] = '\0';

    data->valid = true;

    Serial.println("Weather data parsed successfully:");
    Serial.printf("  Temp: %d°F (H:%d L:%d)\n", data->temp_current, data->temp_high, data->temp_low);
    Serial.printf("  Weather: %s\n", weather_str);
    Serial.printf("  UV: %d (high: %d)\n", data->uv_current, data->uv_high);

    return true;
}

// Disconnect WiFi to save power
void disconnectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disconnected");
}
