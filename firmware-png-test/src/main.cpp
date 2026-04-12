// Minimal PNG display smoke test.
//
// Purpose: confirm that a PNG rendered by the server-side pipeline (Satori +
// resvg, stored in the repo as worker/renderer/preview.png) decodes and
// displays correctly on the real LilyGo T5 4.7" S3 e-paper panel, with
// grayscale shades preserved.
//
// What this firmware does:
//   1. Allocate a 4bpp framebuffer in PSRAM, cleared to white.
//   2. Initialize the EPD and clear the physical panel.
//   3. Decode the embedded preview_png.h into the framebuffer, converting
//      each pixel from RGB565 to an 8-bit luma value and writing through
//      epd_draw_pixel (which quantizes to 4bpp internally).
//   4. Push the framebuffer to the display once.
//   5. Go to sleep in loop(). No refresh, no WiFi, no battery, no overlays.
//
// To update the image: regenerate worker/renderer/preview.png, then
//   python3 scripts/png_to_header.py
// and reflash.

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM (-DBOARD_HAS_PSRAM)"
#endif

#include <Arduino.h>
#include <PNGdec.h>

#include "epd_driver.h"
#include "preview_png.h"

static uint8_t *framebuffer = nullptr;
static PNG png;

// Scanline buffer for RGB565 output. Sized for the panel width with slack.
static uint16_t line_rgb565[EPD_WIDTH + 16];

// PNGdec draw callback: one decoded scanline at a time.
static int png_draw_callback(PNGDRAW *pDraw) {
    // Ask PNGdec to give us the line as RGB565, regardless of the input
    // PNG's native pixel format (RGB, RGBA, indexed, grayscale, whatever).
    // This is the simplest uniform format to process downstream.
    png.getLineAsRGB565(pDraw, line_rgb565, PNG_RGB565_LITTLE_ENDIAN, 0xFFFFFFFF);

    const int y = pDraw->y;
    const int w = pDraw->iWidth;
    if (y < 0 || y >= EPD_HEIGHT) return 1;

    for (int x = 0; x < w && x < EPD_WIDTH; x++) {
        const uint16_t rgb = line_rgb565[x];
        // Expand RGB565 channels back to 8-bit.
        const uint8_t r = ((rgb >> 11) & 0x1F) << 3;
        const uint8_t g = ((rgb >> 5) & 0x3F) << 2;
        const uint8_t b = (rgb & 0x1F) << 3;
        // ITU-R BT.601 luma in fixed point:
        //   Y = 0.299 R + 0.587 G + 0.114 B
        //     ~ (77 R + 150 G + 29 B) / 256
        const uint8_t luma = (uint8_t)((77u * r + 150u * g + 29u * b) >> 8);
        epd_draw_pixel(x, y, luma, framebuffer);
    }
    return 1;
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("PNG display smoke test");
    Serial.printf("Embedded PNG size: %u bytes\n", preview_png_len);

    // Framebuffer: 4bpp, two pixels per byte, initialized to white (0xFF).
    framebuffer = (uint8_t *)ps_calloc(1, (size_t)EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("FATAL: failed to allocate framebuffer");
        while (true) delay(1000);
    }
    memset(framebuffer, 0xFF, (size_t)EPD_WIDTH * EPD_HEIGHT / 2);

    epd_init();
    epd_poweron();
    epd_clear();

    // Decode the embedded PNG into the framebuffer.
    const uint32_t t0 = millis();
    int rc = png.openRAM(
        const_cast<uint8_t *>(preview_png),
        (int)preview_png_len,
        png_draw_callback);
    if (rc != PNG_SUCCESS) {
        Serial.printf("FATAL: png.openRAM failed: %d\n", rc);
        epd_poweroff();
        while (true) delay(1000);
    }

    Serial.printf("PNG header: %dx%d, bpp=%d, pixel type=%d\n",
                  png.getWidth(), png.getHeight(),
                  png.getBpp(), png.getPixelType());

    rc = png.decode(nullptr, 0);
    png.close();
    const uint32_t t1 = millis();
    Serial.printf("Decode returned %d in %lu ms\n", rc, (unsigned long)(t1 - t0));

    // Push the framebuffer to the panel.
    const uint32_t t2 = millis();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    const uint32_t t3 = millis();
    Serial.printf("epd_draw_grayscale_image took %lu ms\n", (unsigned long)(t3 - t2));

    epd_poweroff();
    Serial.println("Done. Frame is on screen.");
}

void loop() {
    delay(60000);
}
