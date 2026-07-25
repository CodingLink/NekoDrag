#!/usr/bin/env python3
"""Generate NekoDrag ICO files from the transparent raster logo source."""

from pathlib import Path
import struct

try:
    from PIL import Image
except ModuleNotFoundError as exc:
    raise SystemExit(
        "Pillow is required to regenerate icons. Install it inside the "
        "project virtual environment with `python3 -m venv .venv`, "
        "`source .venv/bin/activate`, then `pip install pillow`."
    ) from exc


APP_ICON_SIZES = (16, 20, 24, 32, 48, 256)
TRAY_ICON_SIZES = (16, 20, 24)


def load_source(path):
    """Load and validate the square transparent PNG master."""
    with Image.open(path) as image:
        source = image.convert("RGBA")
    if source.width != source.height:
        raise ValueError("The NekoDrag logo source must be square")
    alpha_minimum, alpha_maximum = source.getchannel("A").getextrema()
    if alpha_minimum != 0 or alpha_maximum != 255:
        raise ValueError("The NekoDrag logo source must contain transparency")
    return source


def render_icon(source, size):
    """Render a smooth BGRA bitmap using high-quality downsampling."""
    resampling = getattr(Image, "Resampling", Image).LANCZOS
    rendered = source.resize((size, size), resampling)
    return rendered.tobytes("raw", "BGRA")


def build_bmp_data(width, height, bgra_pixels):
    """Build BITMAPINFOHEADER + XOR mask + AND mask for a 32bpp icon."""
    xor_size = width * height * 4
    row_stride = ((width + 31) // 32) * 4
    and_size = row_stride * height

    header = struct.pack(
        "<IiiHHIIiiII",
        40,             # biSize
        width,          # biWidth
        height * 2,     # biHeight (XOR + AND)
        1,              # biPlanes
        32,             # biBitCount
        0,              # biCompression
        0,              # biSizeImage
        0,              # biXPelsPerMeter
        0,              # biYPelsPerMeter
        0,              # biClrUsed
        0,              # biClrImportant
    )

    xor = bytearray(xor_size)
    for row in range(height):
        source_offset = (height - 1 - row) * width * 4
        target_offset = row * width * 4
        xor[target_offset:target_offset + width * 4] = (
            bgra_pixels[source_offset:source_offset + width * 4]
        )

    and_mask = bytearray(and_size)
    for row in range(height):
        source_row = height - 1 - row
        for column in range(width):
            alpha = bgra_pixels[(source_row * width + column) * 4 + 3]
            if alpha < 128:
                byte_index = row * row_stride + column // 8
                bit = 7 - (column % 8)
                and_mask[byte_index] |= 1 << bit

    return header + xor + and_mask


def write_ico(path, sizes, source):
    """Write an ICO containing one antialiased bitmap per requested size."""
    images = []
    for size in sizes:
        bitmap = build_bmp_data(size, size, render_icon(source, size))
        images.append((size, bitmap))

    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = bytearray()
    data = bytearray()
    for size, bitmap in images:
        entries += struct.pack(
            "<BBBBHHII",
            size if size < 256 else 0,
            size if size < 256 else 0,
            0,
            0,
            1,
            32,
            len(bitmap),
            offset,
        )
        data += bitmap
        offset += len(bitmap)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + entries + data)


def main():
    root = Path(__file__).resolve().parent.parent
    source_path = root / "assets" / "nekodrag.png"
    app_ico = root / "src" / "assets" / "app.ico"
    tray_ico = root / "src" / "assets" / "tray.ico"
    source = load_source(source_path)

    write_ico(app_ico, APP_ICON_SIZES, source)
    write_ico(tray_ico, TRAY_ICON_SIZES, source)
    print(f"Source {source_path}")
    print(f"Generated {app_ico}")
    print(f"Generated {tray_ico}")


if __name__ == "__main__":
    main()
