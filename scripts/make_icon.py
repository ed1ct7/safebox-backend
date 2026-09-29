"""Генерирует src/daemon/safebox.ico: белый замок на тёмно-синем скруглённом квадрате.

Кадры 16/32/48 - BMP (32 бита + маска), 256 - PNG: так понимают и rc.exe, и старые
версии проводника. Нужен Pillow (pip install pillow).

    python scripts/make_icon.py
"""

import io
import struct
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = (16, 32, 48, 256)
MASTER = 1024  # рисуем крупно, потом уменьшаем - края получаются гладкими

BG_TOP = (33, 66, 140)
BG_BOTTOM = (14, 30, 78)
WHITE = (255, 255, 255, 255)


def draw_master() -> Image.Image:
    size = MASTER

    # фон: вертикальный градиент в скруглённом квадрате
    gradient = Image.new("RGBA", (size, size))
    pixels = gradient.load()
    for y in range(size):
        t = y / (size - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM)) + (255,)
        for x in range(size):
            pixels[x, y] = color
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=230, fill=255)
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    image.paste(gradient, (0, 0), mask)

    # замок рисуем маской: вырезы (внутри дужки, скважина) пропускают фон
    lock = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(lock)
    # дужка: "П" толщиной 66, нижние концы прячутся под корпусом
    draw.rounded_rectangle((336, 214, 688, 600), radius=176, fill=255)
    draw.rounded_rectangle((402, 280, 622, 620), radius=110, fill=0)
    # корпус и скважина
    draw.rounded_rectangle((268, 452, 756, 812), radius=64, fill=255)
    draw.ellipse((468, 570, 556, 658), fill=0)
    draw.rounded_rectangle((490, 630, 534, 736), radius=22, fill=0)
    image.paste(WHITE, (0, 0), lock)
    return image


def bmp_frame(image: Image.Image) -> bytes:
    """DIB для ICO: заголовок, пиксели BGRA снизу вверх, пустая AND-маска."""
    width, height = image.size
    header = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    rows = []
    for y in range(height - 1, -1, -1):
        row = bytearray()
        for x in range(width):
            r, g, b, a = image.getpixel((x, y))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    mask_row = b"\x00" * (((width + 31) // 32) * 4)
    return header + b"".join(rows) + mask_row * height


def png_frame(image: Image.Image) -> bytes:
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def build_ico(master: Image.Image) -> bytes:
    frames = []
    for size in SIZES:
        image = master.resize((size, size), Image.LANCZOS)
        frames.append((size, png_frame(image) if size >= 256 else bmp_frame(image)))

    out = bytearray(struct.pack("<HHH", 0, 1, len(frames)))
    offset = 6 + 16 * len(frames)
    for size, data in frames:
        dimension = 0 if size >= 256 else size  # 0 означает 256
        out += struct.pack("<BBBBHHII", dimension, dimension, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    for _, data in frames:
        out += data
    return bytes(out)


def main() -> None:
    target = Path(__file__).resolve().parent.parent / "src" / "daemon" / "safebox.ico"
    target.write_bytes(build_ico(draw_master()))
    print(f"записано {target} ({target.stat().st_size} байт)")


if __name__ == "__main__":
    main()
