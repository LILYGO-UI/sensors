"""Generate the original Sensors launcher icon using Pillow."""

from pathlib import Path

from PIL import Image, ImageDraw


def generate_icon():
    scale = 4
    image = Image.new("RGBA", (128 * scale, 128 * scale), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def rect(box, color, radius=0):
        draw.rounded_rectangle(tuple(int(value * scale) for value in box),
                               radius=radius * scale, fill=color)

    rect((2, 2, 126, 126), "#173F43", 26)
    for offset in (43, 64, 85):
        rect((offset - 3, 18, offset + 3, 33), "#A3DCD4", 2)
        rect((offset - 3, 95, offset + 3, 110), "#A3DCD4", 2)
        rect((18, offset - 3, 33, offset + 3), "#A3DCD4", 2)
        rect((95, offset - 3, 110, offset + 3), "#A3DCD4", 2)
    rect((29, 29, 99, 99), "#F4FAF8", 12)
    points = [(39, 67), (48, 67), (55, 50), (64, 79), (74, 57), (80, 67), (89, 67)]
    draw.line([(x * scale, y * scale) for x, y in points],
              fill="#138F83", width=5 * scale, joint="curve")
    rect((79, 36, 90, 47), "#E9B94D", 5)
    output = Path(__file__).resolve().parents[1] / "assets" / "app-icon.png"
    output.parent.mkdir(parents=True, exist_ok=True)
    image.resize((128, 128), Image.Resampling.LANCZOS).save(output)


if __name__ == "__main__":
    generate_icon()
