#!/usr/bin/env python3
"""
Convert PNG weather icons to EPD47 4-bit grayscale bitmap format.

The EPD47 library uses 4-bit grayscale where:
- 0x00 = black
- 0xFF = white
- Values in between are gray levels

Format: Each byte contains 2 pixels (4 bits each, nibble-packed)
"""

import sys
from PIL import Image
import os

def png_to_epd47_bitmap(png_path, output_size=200):
    """
    Convert PNG to EPD47 4-bit grayscale bitmap array.

    Args:
        png_path: Path to PNG file
        output_size: Target size (will be square)

    Returns:
        C array definition string
    """
    # Load and process image
    img = Image.open(png_path)

    # Resize to target size with high-quality resampling
    img = img.resize((output_size, output_size), Image.Resampling.LANCZOS)

    # Convert to grayscale
    if img.mode != 'L':
        # If RGBA, composite over white background first
        if img.mode == 'RGBA':
            background = Image.new('RGB', img.size, (255, 255, 255))
            background.paste(img, mask=img.split()[3])  # Alpha channel as mask
            img = background.convert('L')
        else:
            img = img.convert('L')

    # Convert to 4-bit (0-15 range)
    pixels = []
    for y in range(output_size):
        for x in range(output_size):
            gray8 = img.getpixel((x, y))
            # Convert 8-bit (0-255) to 4-bit (0-15), inverted for e-paper
            # 0 = black, 15 = white in 4-bit
            gray4 = gray8 >> 4  # Divide by 16 to get 0-15
            pixels.append(gray4)

    # Pack into bytes (2 pixels per byte)
    byte_array = []
    for i in range(0, len(pixels), 2):
        high_nibble = pixels[i]
        low_nibble = pixels[i + 1] if i + 1 < len(pixels) else 0
        byte_val = (high_nibble << 4) | low_nibble
        byte_array.append(byte_val)

    # Generate C array
    icon_name = os.path.splitext(os.path.basename(png_path))[0]
    # Replace hyphens with underscores for valid C identifier
    icon_name_c = icon_name.replace('-', '_')

    # Format as C array with 12 bytes per line
    lines = []
    lines.append(f"// {icon_name}.png - {output_size}x{output_size} 4-bit grayscale")
    lines.append(f"const uint8_t icon_{icon_name_c}_{output_size}[] = {{")

    for i in range(0, len(byte_array), 12):
        chunk = byte_array[i:i+12]
        hex_values = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f"    {hex_values},")

    lines.append("};")
    lines.append("")

    return '\n'.join(lines)

def main():
    icon_dir = "/Users/leighherbert/Desktop/weather_icons"
    output_file = "src/weather_icon_bitmaps.h"

    icons = [
        "sun.png",
        "moon.png",
        "partly.png",
        "partly-night.png",
        "cloud.png",
        "rainy.png",
        "snowflake.png",
        "lighting.png",
        "fog.png"
    ]

    output_lines = [
        "#pragma once",
        "#include <stdint.h>",
        "",
        "// Weather icon bitmaps - 200x200 4-bit grayscale",
        "// Generated from PNG files",
        ""
    ]

    for icon in icons:
        png_path = os.path.join(icon_dir, icon)
        if os.path.exists(png_path):
            print(f"Converting {icon}...")
            bitmap_code = png_to_epd47_bitmap(png_path, output_size=200)
            output_lines.append(bitmap_code)
        else:
            print(f"Warning: {png_path} not found")

    # Write output
    with open(output_file, 'w') as f:
        f.write('\n'.join(output_lines))

    print(f"\nGenerated {output_file}")
    print(f"Total size: {os.path.getsize(output_file)} bytes")

if __name__ == "__main__":
    main()
