// Button probe — temporary firmware to identify which GPIOs the user-accessible
// buttons on the LilyGo T5 4.7" S3 board are wired to.
//
// Flash, open serial monitor, press each physical button one at a time, and
// note which GPIO logs "PRESSED". After identifying buttons, re-flash the
// real firmware (firmware-png/) to restore normal operation.
//
// Pins are configured INPUT_PULLUP, so a button press = pin pulled to GND.

#include <Arduino.h>

// Candidate GPIOs to monitor. Skips pins that are unsafe to reconfigure:
//   19, 20  → USB-JTAG D-/D+ (would break Serial-over-USB if reconfigured)
//   45, 46  → strapping pins (boot behavior — leave alone)
//   43, 44  → UART0 TX/RX (used by some serial paths)
// Everything else exposed on the LilyGo header is fair game; reading them as
// input does no harm even if the EPD parallel interface uses some during
// normal operation (we never init the EPD here).
static const int CANDIDATES[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
    47, 48,
};
static const int N = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);

static bool prev[N];

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("=== Button probe ===");
    Serial.printf("Monitoring %d GPIOs as INPUT_PULLUP.\n", N);
    Serial.println("Press each physical button — pin numbers will print on transition.");
    Serial.println();

    for (int i = 0; i < N; i++) {
        pinMode(CANDIDATES[i], INPUT_PULLUP);
    }
    delay(200);

    // Establish baseline. Flag any pin that's already LOW — could be a
    // hardwired-low pin or a button being held during boot.
    for (int i = 0; i < N; i++) {
        prev[i] = digitalRead(CANDIDATES[i]);
        if (!prev[i]) {
            Serial.printf("  GPIO %2d already LOW at startup\n", CANDIDATES[i]);
        }
    }
    Serial.println("\nReady. Press buttons now.\n");
}

void loop() {
    for (int i = 0; i < N; i++) {
        bool now = digitalRead(CANDIDATES[i]);
        if (now != prev[i]) {
            Serial.printf("GPIO %2d: %s\n", CANDIDATES[i], now ? "released" : "PRESSED");
            prev[i] = now;
        }
    }
    delay(20);
}
