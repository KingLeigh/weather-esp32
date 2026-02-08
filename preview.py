#!/usr/bin/env python3
"""
Preview the weather display layout as a PNG image.
Simulates the 960x540 4-bit grayscale e-paper display.
"""
from PIL import Image, ImageDraw, ImageFont
import math
import os

WIDTH, HEIGHT = 960, 540
FONT_TTF = "/tmp/FiraSans-Bold.ttf"

# --- Hardcoded test data (mirrors main.cpp) ---
current_temp = 72
high_temp = 78
low_temp = 65
icon_type = "sunny"  # sunny, cloudy, partly_cloudy, rainy, snowy
precip_pct = [0, 0, 10, 20, 45, 60, 40, 20, 10, 5, 0, 0]
uv_current = 6
uv_high = 9
battery_percent = 73
last_updated = "2:30:42 PM"

# --- Load fonts at sizes matching the generated headers ---
# Large: 48pt at 150dpi. PIL uses 72dpi by default, so scale: 48 * 150/72 = 100
# Medium: 29pt at 150dpi -> 29 * 150/72 = 60
# Small (FiraSans built-in): ~20px -> roughly 20 * 150/72 = 42, but built-in is smaller
font_large = ImageFont.truetype(FONT_TTF, 100)
font_medium = ImageFont.truetype(FONT_TTF, 60)
font_small = ImageFont.truetype(FONT_TTF, 28)


ICON_COLOR = 80  # soft gray (0=black, 255=white)


def draw_sun(draw, cx, cy, radius, color=ICON_COLOR):
    """Sun: filled circle + 8 radiating lines."""
    draw.ellipse([cx - radius, cy - radius, cx + radius, cy + radius], fill=color)
    inner = radius + 6
    outer = radius + 22
    for i in range(8):
        angle = i * math.pi / 4.0
        x0 = cx + int(inner * math.cos(angle))
        y0 = cy + int(inner * math.sin(angle))
        x1 = cx + int(outer * math.cos(angle))
        y1 = cy + int(outer * math.sin(angle))
        draw.line([(x0, y0), (x1, y1)], fill=color, width=3)


def draw_cloud(draw, cx, cy, size, color=ICON_COLOR):
    """Cloud: many overlapping circles for a puffy, cartoony look."""
    s = size / 100.0

    for offset_x, r in [(-28, 18), (-10, 20), (10, 20), (28, 18)]:
        rx = cx + int(offset_x * s)
        ry = cy + int(8 * s)
        rr = int(r * s)
        draw.ellipse([rx - rr, ry - rr, rx + rr, ry + rr], fill=color)

    for offset_x, offset_y, r in [(-22, -8, 20), (0, -12, 24), (22, -6, 19)]:
        rx = cx + int(offset_x * s)
        ry = cy + int(offset_y * s)
        rr = int(r * s)
        draw.ellipse([rx - rr, ry - rr, rx + rr, ry + rr], fill=color)

    rx = cx + int(-4 * s)
    ry = cy - int(26 * s)
    rr = int(20 * s)
    draw.ellipse([rx - rr, ry - rr, rx + rr, ry + rr], fill=color)


def draw_rain(draw, cx, cy, size, color=ICON_COLOR):
    """Cloud + angled rain lines."""
    draw_cloud(draw, cx, cy - size * 15 // 100, size, color)
    drop_top = cy + size * 15 // 100
    drop_len = size * 18 // 100
    for i in range(5):
        dx = cx - size * 25 // 100 + i * size * 13 // 100
        y_off = (i % 2) * 8
        draw.line([(dx, drop_top + y_off), (dx - 4, drop_top + drop_len + y_off)], fill=color, width=2)


def draw_snow(draw, cx, cy, size, color=ICON_COLOR):
    """Cloud + snowflake dots."""
    draw_cloud(draw, cx, cy - size * 15 // 100, size, color)
    snow_y1 = cy + size * 18 // 100
    snow_y2 = cy + size * 32 // 100
    for i in range(4):
        dx = cx - size * 22 // 100 + i * size * 15 // 100
        draw.ellipse([dx - 3, snow_y1 - 3, dx + 3, snow_y1 + 3], fill=color)
    for i in range(3):
        dx = cx - size * 15 // 100 + i * size * 15 // 100
        draw.ellipse([dx - 3, snow_y2 - 3, dx + 3, snow_y2 + 3], fill=color)


def draw_partly_cloudy(draw, cx, cy, size, color=ICON_COLOR):
    """Sun peeking behind cloud."""
    sun_r = size * 18 // 100
    draw_sun(draw, cx - size * 15 // 100, cy - size * 18 // 100, sun_r, color)

    cloud_cx = cx + size * 8 // 100
    cloud_cy = cy + size * 8 // 100
    r1 = size * 25 // 100
    r2 = size * 18 // 100
    r3 = size * 20 // 100
    base_w = size * 58 // 100
    base_h = size * 12 // 100

    # White fill behind cloud
    draw.rectangle([cloud_cx - base_w // 2 - 2, cloud_cy - r1 // 2,
                     cloud_cx + base_w // 2 + 2, cloud_cy + r1 + base_h + 4], fill=255)
    draw.ellipse([cloud_cx - r1 - 3, cloud_cy - r1 // 3 - r1 - 3,
                   cloud_cx + r1 + 3, cloud_cy - r1 // 3 + r1 + 3], fill=255)
    draw.ellipse([cloud_cx - size * 14 // 100 - r2 - 3, cloud_cy - r2 - 3,
                   cloud_cx - size * 14 // 100 + r2 + 3, cloud_cy + r2 + 3], fill=255)
    draw.ellipse([cloud_cx + size * 13 // 100 - r3 - 3, cloud_cy - r3 // 4 - r3 - 3,
                   cloud_cx + size * 13 // 100 + r3 + 3, cloud_cy - r3 // 4 + r3 + 3], fill=255)

    draw_cloud(draw, cloud_cx, cloud_cy, size * 85 // 100, color)


def draw_weather_icon(draw, icon, cx, cy, size):
    """Dispatch to icon drawing function."""
    if icon == "sunny":
        draw_sun(draw, cx, cy, size * 30 // 100)
    elif icon == "cloudy":
        draw_cloud(draw, cx, cy, size)
    elif icon == "partly_cloudy":
        draw_partly_cloudy(draw, cx, cy, size)
    elif icon == "rainy":
        draw_rain(draw, cx, cy, size)
    elif icon == "snowy":
        draw_snow(draw, cx, cy, size)


def draw_precip_chart(draw, x, y, w, h, data, count):
    """Precipitation time series line chart, 0-100% y-axis."""
    label_h = 30
    title_h = 25
    chart_h = h - label_h - title_h
    chart_y = y + title_h
    chart_bottom = chart_y + chart_h

    # Title
    draw.text((x, y), "Precipitation next 12h", fill=0, font=font_small)

    # Y-axis labels and gridlines at 0%, 25%, 50%, 75%, 100%
    for pct in [0, 25, 50, 75, 100]:
        gy = chart_bottom - (chart_h * pct // 100)
        color = 192 if pct not in [0, 100] else 160
        draw.line([(x, gy), (x + w, gy)], fill=color, width=1)

    # Build line points
    step = w / (count - 1)
    points = []
    for i in range(count):
        px = x + int(i * step)
        py = chart_bottom - (chart_h * data[i] // 100)
        points.append((px, py))

    # Fill area under the line
    fill_points = list(points) + [(points[-1][0], chart_bottom), (points[0][0], chart_bottom)]
    draw.polygon(fill_points, fill=220)

    # Draw the line
    draw.line(points, fill=0, width=2)

    # Hour labels along x-axis (first, middle, last only)
    for i in [0, count // 2, count - 1]:
        hour = (12 + i) % 12
        if hour == 0:
            hour = 12
        label = str(hour)
        lx = x + int(i * step)
        bbox = draw.textbbox((0, 0), label, font=font_small)
        lw = bbox[2] - bbox[0]
        draw.text((lx - lw // 2, chart_bottom + 4), label, fill=0, font=font_small)


def draw_battery_icon(draw, x, y, percent):
    """Draw a small battery icon with fill level. x, y = top-left corner."""
    w, h = 20, 10
    tip_w = 2

    # Battery body outline
    draw.rectangle([x, y, x + w, y + h], outline=160, fill=255)
    # Battery tip
    draw.rectangle([x + w, y + 3, x + w + tip_w, y + h - 3], fill=160)

    # Fill level (inside body)
    fill_w = int((w - 2) * percent / 100)
    if fill_w > 0:
        draw.rectangle([x + 1, y + 1, x + 1 + fill_w, y + h - 1], fill=80)


def draw_uv_index(draw, x, y, uv_now, uv_hi):
    """Draw UV index panel with small sun icon and current/high values."""
    # Small sun icon
    sun_cx = x + 30
    sun_cy = y + 30
    sun_r = 14
    draw.ellipse([sun_cx - sun_r, sun_cy - sun_r, sun_cx + sun_r, sun_cy + sun_r], fill=0)
    for i in range(8):
        angle = i * math.pi / 4.0
        inner = sun_r + 4
        outer = sun_r + 12
        x0 = sun_cx + int(inner * math.cos(angle))
        y0 = sun_cy + int(inner * math.sin(angle))
        x1 = sun_cx + int(outer * math.cos(angle))
        y1 = sun_cy + int(outer * math.sin(angle))
        draw.line([(x0, y0), (x1, y1)], fill=0, width=2)

    # "UV Index" label
    draw.text((x + 60, y + 10), "UV Index", fill=0, font=font_small)

    # Current and High values (small font for labels, medium for numbers)
    draw.text((x, y + 75), "Now", fill=0, font=font_small)
    draw.text((x + 70, y + 65), str(uv_now), fill=0, font=font_medium)
    draw.text((x, y + 135), "High", fill=0, font=font_small)
    draw.text((x + 70, y + 125), str(uv_hi), fill=0, font=font_medium)


def main_weather():
    """Normal weather display layout."""
    img = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(img)

    # Current temperature (large font, top-left)
    draw.text((50, 30), f"{current_temp}\u00B0", fill=0, font=font_large)

    # Weather icon (top-right)
    draw_weather_icon(draw, icon_type, 780, 110, 200)

    # High / Low temps (medium font)
    hi_text = f"H: {high_temp}\u00B0"
    lo_text = f"L: {low_temp}\u00B0"
    draw.text((50, 170), hi_text, fill=0, font=font_medium)
    # Measure hi_text width to position lo_text after it
    hi_bbox = draw.textbbox((0, 0), hi_text, font=font_medium)
    hi_w = hi_bbox[2] - hi_bbox[0]
    draw.text((50 + hi_w + 30, 170), lo_text, fill=0, font=font_medium)

    # Divider line
    draw.line([(40, 265), (920, 265)], fill=128, width=1)

    # Precipitation chart (half width)
    draw_precip_chart(draw, 40, 280, 440, 200, precip_pct, 12)

    # UV Index (right half of lower section)
    draw_uv_index(draw, 540, 280, uv_current, uv_high)

    # Timestamp with battery icon (lower-right corner, subtle)
    bbox = draw.textbbox((0, 0), last_updated, font=font_small)
    tw = bbox[2] - bbox[0]
    timestamp_x = WIDTH - tw - 20
    timestamp_y = HEIGHT - 35

    # Battery icon to the left of timestamp (centered vertically)
    battery_x = timestamp_x - 30
    battery_y = timestamp_y - 5
    draw_battery_icon(draw, battery_x, battery_y, battery_percent)

    draw.text((timestamp_x, timestamp_y), last_updated, fill=160, font=font_small)

    out_path = os.path.join(os.path.dirname(__file__), "preview.png")
    img.save(out_path)
    print(f"Preview saved to {out_path}")


def main_icons():
    """Show all 5 weather icons side by side."""
    img = Image.new("L", (WIDTH, HEIGHT), 255)
    draw = ImageDraw.Draw(img)

    icons = ["sunny", "cloudy", "partly_cloudy", "rainy", "snowy"]
    labels = ["Sunny", "Cloudy", "Partly Cloudy", "Rainy", "Snowy"]
    spacing = WIDTH // len(icons)

    for i, (ic, label) in enumerate(zip(icons, labels)):
        cx = spacing // 2 + i * spacing
        cy = HEIGHT // 2 - 30
        draw_weather_icon(draw, ic, cx, cy, 160)
        bbox = draw.textbbox((0, 0), label, font=font_small)
        lw = bbox[2] - bbox[0]
        draw.text((cx - lw // 2, cy + 110), label, fill=0, font=font_small)

    out_path = os.path.join(os.path.dirname(__file__), "preview.png")
    img.save(out_path)
    print(f"Preview saved to {out_path}")


def main():
    import sys
    if len(sys.argv) > 1 and sys.argv[1] == "icons":
        main_icons()
    else:
        main_weather()


if __name__ == "__main__":
    main()
