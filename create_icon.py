import math
import os
from PIL import Image, ImageDraw, ImageFilter

def create_manga_icon():
    os.makedirs('assets', exist_ok=True)
    size = 256
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # 1. Base Rounded Square with Gradient
    # Background: Deep vibrant indigo/violet to dark cyan
    corner_radius = 48
    bg = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    bg_draw = ImageDraw.Draw(bg)
    
    # Draw soft outer glow / drop shadow
    shadow = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    s_draw = ImageDraw.Draw(shadow)
    s_draw.rounded_rectangle([12, 16, 244, 248], radius=corner_radius, fill=(10, 10, 25, 120))
    shadow = shadow.filter(ImageFilter.GaussianBlur(8))
    img.paste(shadow, (0, 0), shadow)

    # Gradient squircle
    for y in range(size):
        ratio = y / size
        # Gradient from #6366f1 (Indigo 500) to #0f172a (Slate 900) with cyan accent
        r = int(99 * (1 - ratio) + 15 * ratio)
        g = int(102 * (1 - ratio) + 23 * ratio + 30 * math.sin(ratio * math.pi))
        b = int(241 * (1 - ratio) + 42 * ratio + 80 * math.sin(ratio * math.pi))
        bg_draw.line([(0, y), (size, y)], fill=(r, g, b, 255))

    # Mask with rounded rectangle
    mask = Image.new('L', (size, size), 0)
    m_draw = ImageDraw.Draw(mask)
    m_draw.rounded_rectangle([14, 14, 242, 242], radius=corner_radius, fill=255)
    img.paste(bg, (0, 0), mask)

    # Border stroke
    overlay = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    o_draw = ImageDraw.Draw(overlay)
    o_draw.rounded_rectangle([14, 14, 242, 242], radius=corner_radius, outline=(255, 255, 255, 60), width=3)
    o_draw.rounded_rectangle([16, 16, 240, 240], radius=corner_radius - 2, outline=(56, 189, 248, 140), width=2)
    img.paste(overlay, (0, 0), overlay)

    # 2. Manga Book / Pages Graphic
    # Open manga book tilted slightly, stylized in white & cyan
    pages = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    p_draw = ImageDraw.Draw(pages)

    # Left Page
    left_poly = [(48, 148), (120, 164), (120, 92), (48, 76)]
    # Right Page
    right_poly = [(208, 148), (136, 164), (136, 92), (208, 76)]

    p_draw.polygon(left_poly, fill=(240, 244, 255, 230))
    p_draw.polygon(right_poly, fill=(255, 255, 255, 245))

    # Manga panel lines on left page
    p_draw.line([(60, 95), (108, 105)], fill=(129, 140, 248, 200), width=3)
    p_draw.line([(60, 115), (82, 120)], fill=(129, 140, 248, 160), width=2)
    p_draw.line([(88, 121), (108, 125)], fill=(129, 140, 248, 160), width=2)
    p_draw.line([(60, 135), (108, 145)], fill=(129, 140, 248, 180), width=2)

    # Manga panel lines on right page
    p_draw.line([(148, 105), (196, 95)], fill=(56, 189, 248, 220), width=3)
    p_draw.line([(148, 125), (170, 120)], fill=(56, 189, 248, 180), width=2)
    p_draw.line([(176, 118), (196, 114)], fill=(56, 189, 248, 180), width=2)
    p_draw.line([(148, 145), (196, 135)], fill=(56, 189, 248, 180), width=2)

    # Book spine fold highlight
    p_draw.polygon([(120, 92), (136, 92), (136, 164), (120, 164)], fill=(99, 102, 241, 230))

    img.paste(pages, (0, 0), pages)

    # 3. Dynamic Download Arrow & Tray (Stylized Manga Cyber Accent)
    arrow = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    a_draw = ImageDraw.Draw(arrow)

    # Arrow shadow / glow
    glow = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    g_draw = ImageDraw.Draw(glow)

    arrow_poly = [
        (116, 52), (140, 52), (140, 105), (164, 105),
        (128, 142), (92, 105), (116, 105)
    ]
    g_draw.polygon(arrow_poly, fill=(56, 189, 248, 180))
    glow = glow.filter(ImageFilter.GaussianBlur(6))
    img.paste(glow, (0, 0), glow)

    # Crisp Arrow body with vibrant gradient or cyan/white
    a_draw.polygon(arrow_poly, fill=(34, 211, 238, 255), outline=(255, 255, 255, 230), width=2)

    # Lower tray / download cradle
    tray_poly = [
        (68, 186), (82, 186), (82, 196), (174, 196),
        (174, 186), (188, 186), (188, 208), (68, 208)
    ]
    a_draw.polygon(tray_poly, fill=(255, 255, 255, 240), outline=(56, 189, 248, 200), width=1)

    # 4. AI Sparkle Star in top right
    def draw_sparkle(cx, cy, r_outer, r_inner, color):
        pts = []
        for i in range(8):
            ang = i * math.pi / 4
            r = r_outer if i % 2 == 0 else r_inner
            pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
        a_draw.polygon(pts, fill=color)

    draw_sparkle(204, 52, 18, 5, (253, 224, 71, 255))
    draw_sparkle(178, 36, 10, 3, (254, 240, 138, 230))

    img.paste(arrow, (0, 0), arrow)

    # Save PNG
    img.save('assets/app_icon.png', format='PNG')

    # Save Multi-resolution ICO: 16, 24, 32, 48, 64, 128, 256
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save('assets/app_icon.ico', format='ICO', sizes=sizes)
    print("Icon generated successfully at assets/app_icon.ico and assets/app_icon.png")

if __name__ == '__main__':
    create_manga_icon()
