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

    // Ensure clean WiFi state before connecting
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);

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
        Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());

        // Set DNS servers AFTER connection (more reliable on ESP32-S3)
        IPAddress dns1(8, 8, 8, 8);      // Google DNS
        IPAddress dns2(1, 1, 1, 1);      // Cloudflare DNS

        // Get current network config
        IPAddress ip = WiFi.localIP();
        IPAddress gateway = WiFi.gatewayIP();
        IPAddress subnet = WiFi.subnetMask();

        // Reconfigure with custom DNS
        WiFi.config(ip, gateway, subnet, dns1, dns2);

        Serial.printf("DNS 1: %s\n", WiFi.dnsIP(0).toString().c_str());
        Serial.printf("DNS 2: %s\n", WiFi.dnsIP(1).toString().c_str());
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
    if (strcmp(weather_str, "thunderstorm") == 0) return THUNDERSTORM;
    if (strcmp(weather_str, "fog") == 0) return FOG;
    return PARTLY_CLOUDY;  // default
}

// Fetch weather data from API with retry logic
bool fetchWeatherData(WeatherData* data) {
    if (!data) return false;

    data->valid = false;

    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_MS = 2000;

    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        Serial.printf("Fetching weather (attempt %d/%d) from: %s\n", attempt, MAX_RETRIES, WEATHER_API_URL);

        HTTPClient http;
        http.begin(WEATHER_API_URL);
        http.setTimeout(15000);  // 15 second timeout (increased from 10)
        http.setConnectTimeout(10000);  // 10 second connection timeout

        int httpCode = http.GET();

        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            http.end();

            Serial.printf("Received %d bytes\n", payload.length());

            // Parse JSON
            JsonDocument doc;
            DeserializationError error = deserializeJson(doc, payload);

            if (error) {
                Serial.printf("JSON parse error: %s\n", error.c_str());
                if (attempt < MAX_RETRIES) {
                    Serial.printf("Retrying in %d ms...\n", RETRY_DELAY_MS);
                    delay(RETRY_DELAY_MS);
                    continue;
                }
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

        // HTTP request failed - provide detailed diagnostics
        Serial.println("========== HTTP REQUEST FAILED ==========");
        Serial.printf("HTTP Code: %d\n", httpCode);

        if (httpCode > 0) {
            // Positive codes are HTTP status codes
            Serial.printf("HTTP Status: %s\n", http.errorToString(httpCode).c_str());

            // Try to get response body for debugging
            String errorBody = http.getString();
            if (errorBody.length() > 0 && errorBody.length() < 500) {
                Serial.printf("Response body: %s\n", errorBody.c_str());
            } else if (errorBody.length() > 0) {
                Serial.printf("Response body (first 500 chars): %s...\n", errorBody.substring(0, 500).c_str());
            }

            // Common HTTP error explanations
            if (httpCode == 404) {
                Serial.println("ERROR: API endpoint not found (404)");
            } else if (httpCode == 500 || httpCode == 502 || httpCode == 503) {
                Serial.println("ERROR: Server error - Worker may be down or misconfigured");
            } else if (httpCode == 403) {
                Serial.println("ERROR: Access forbidden - check Worker routes/permissions");
            } else if (httpCode == 429) {
                Serial.println("ERROR: Rate limited");
            }
        } else {
            // Negative codes are client errors
            Serial.printf("Client Error: %s\n", http.errorToString(httpCode).c_str());

            if (httpCode == -1) {
                Serial.println("ERROR: Connection failed - DNS or network issue");
            } else if (httpCode == -2) {
                Serial.println("ERROR: Connection lost during request");
            } else if (httpCode == -3) {
                Serial.println("ERROR: HTTP header read timeout");
            } else if (httpCode == -11) {
                Serial.println("ERROR: Read timeout - server not responding");
            }
        }
        Serial.println("=========================================");

        http.end();

        if (attempt < MAX_RETRIES) {
            Serial.printf("Retrying in %d ms...\n", RETRY_DELAY_MS);
            delay(RETRY_DELAY_MS);
        }
    }

    Serial.println("All retry attempts failed");
    return false;
}

// Disconnect WiFi to save power
void disconnectWiFi() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disconnected");
}
