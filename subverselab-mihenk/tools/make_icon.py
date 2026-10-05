"""Builds Assets/AppIcon_1024.png: the macOS app icon for MIHENK (dark green squircle, cream mark, gold rim)."""
from PIL import Image, ImageDraw, ImageFilter
S = 1024
BODY = 824            # Apple grid: 824 px body inside 1024 canvas, 100 px margin
M = (S - BODY) // 2
R = 185               # corner radius of the body
top, bot = (0x0d, 0x3b, 0x33), (0x02, 0x15, 0x12)
canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))

# soft drop shadow
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).rounded_rectangle((M, M + 14, M + BODY, M + BODY + 14), R, fill=(0, 0, 0, 120))
canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(18)))

# body: vertical gradient, masked to the rounded square
grad = Image.new("RGBA", (S, S))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = min(1.0, max(0.0, (y - M) / BODY))
    gd.line([(0, y), (S, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bot)) + (255,))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((M, M, M + BODY, M + BODY), R, fill=255)
canvas.paste(grad, (0, 0), mask)

# thin gold rim inside the edge
ImageDraw.Draw(canvas).rounded_rectangle((M + 18, M + 18, M + BODY - 18, M + BODY - 18), R - 18, outline=(0xF6, 0xD5, 0x8E, 150), width=5)

# cream mark, centred, ~68 % of the body width
mark = Image.open("Assets/mark_cream.png").convert("RGBA")
w = int(BODY * 0.68); h = int(mark.height * w / mark.width)
mark = mark.resize((w, h), Image.LANCZOS)
canvas.alpha_composite(mark, ((S - w) // 2 + 8, (S - h) // 2))
canvas.save("Assets/AppIcon_1024.png")
print("Assets/AppIcon_1024.png", canvas.size)
