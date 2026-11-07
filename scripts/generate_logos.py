import os
from PIL import Image, ImageDraw

art_dir = r"C:\Users\Reyansh\.gemini\antigravity\brain\708010d5-fa11-4103-8002-855a22b75d58"
os.makedirs(art_dir, exist_ok=True)

# ── 1. Atlas OS Logo (48x48 Pixel Art & 192x192 Scaled) ──────────────────────
def generate_atlas_logo():
    # 48x48 base
    img = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Palette
    C_CYAN_HI  = (0, 240, 255, 255)
    C_CYAN_MID = (0, 180, 220, 255)
    C_CYAN_DK  = (0, 100, 140, 255)
    C_AMBER    = (255, 180, 0, 255)
    C_AMBER_LT = (255, 220, 80, 255)
    C_WHITE    = (255, 255, 255, 255)

    # 1. Back half of orbital ring (ellipse passing behind 'A')
    # Center (24, 24), rx=20, ry=8
    for angle_deg in range(160, 380, 2):
        import math
        rad = math.radians(angle_deg)
        x = int(24 + 20 * math.cos(rad))
        y = int(23 + 7 * math.sin(rad))
        if 0 <= x < 48 and 0 <= y < 48:
            img.putpixel((x, y), C_AMBER)
            if y + 1 < 48: img.putpixel((x, y + 1), (180, 110, 0, 255))

    # 2. Main 'A' Structure - Bold Cybernetic Chevron
    # Apex at (24, 6)
    # Left leg down to (9, 41)
    # Right leg down to (39, 41)
    for t in range(36):
        pct = t / 35.0
        # Left leg
        lx = int(24 - 15 * pct)
        ly = int(6 + 35 * pct)
        for w in range(4):
            if 0 <= lx + w < 48 and 0 <= ly < 48:
                col = C_CYAN_HI if w == 0 else (C_CYAN_MID if w < 3 else C_CYAN_DK)
                img.putpixel((lx + w, ly), col)
        # Right leg
        rx = int(24 + 15 * pct)
        ry = int(6 + 35 * pct)
        for w in range(4):
            if 0 <= rx - w < 48 and 0 <= ry < 48:
                col = C_CYAN_HI if w == 0 else (C_CYAN_MID if w < 3 else C_CYAN_DK)
                img.putpixel((rx - w, ry), col)

    # Crossbar at y=27..29, from x=17 to x=31
    for y in range(27, 30):
        for x in range(16, 33):
            col = C_CYAN_HI if y == 27 else C_CYAN_MID
            img.putpixel((x, y), col)

    # Central Core Crystal (Diamond inside upper A)
    crystal_pts = [(24, 15), (27, 21), (24, 25), (21, 21)]
    draw.polygon(crystal_pts, fill=C_AMBER_LT, outline=C_WHITE)

    # 3. Front half of orbital ring (ellipse passing IN FRONT of 'A')
    for angle_deg in range(0, 180, 2):
        import math
        rad = math.radians(angle_deg)
        x = int(24 + 20 * math.cos(rad))
        y = int(25 + 7 * math.sin(rad))
        if 0 <= x < 48 and 0 <= y < 48:
            img.putpixel((x, y), C_AMBER_LT)
            if y + 1 < 48: img.putpixel((x, y + 1), C_AMBER)

    # Feet of the A
    for dx in range(6):
        if 7 + dx < 48:
            img.putpixel((7 + dx, 41), C_CYAN_HI)
            img.putpixel((7 + dx, 42), C_CYAN_DK)
        if 36 + dx < 48:
            img.putpixel((36 + dx, 41), C_CYAN_HI)
            img.putpixel((36 + dx, 42), C_CYAN_DK)

    # Save 48x48
    p48 = os.path.join(art_dir, "atlas_logo_48.png")
    img.save(p48)

    # Scale 4x with nearest neighbor for crisp display preview
    scaled = img.resize((192, 192), Image.NEAREST)
    scaled.save(os.path.join(art_dir, "atlas_logo_preview.png"))
    return p48

# ── 2. APKG Store Logo (48x48 Pixel Art & 192x192 Scaled) ───────────────────
def generate_apkg_logo():
    img = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    C_LIME_HI  = (70, 240, 110, 255)
    C_LIME_MID = (40, 200, 80, 255)
    C_LIME_DK  = (20, 130, 50, 255)
    C_CYAN_ACC = (0, 210, 255, 255)
    C_GOLD     = (255, 210, 20, 255)
    C_WHITE    = (255, 255, 255, 255)

    # Isometric Crate/Cartridge:
    # Center apex (24, 8)
    # Right corner (42, 18)
    # Left corner  (6, 18)
    # Bottom corner of top face (24, 28)
    # Bottom apex (24, 43)
    # Bottom left (6, 33)
    # Bottom right (42, 33)

    # Top Face (Lighter shade)
    top_poly = [(24, 8), (42, 18), (24, 28), (6, 18)]
    draw.polygon(top_poly, fill=(35, 60, 50, 255), outline=C_CYAN_ACC)

    # Left Face (Mid shade)
    left_poly = [(6, 18), (24, 28), (24, 43), (6, 33)]
    draw.polygon(left_poly, fill=C_LIME_DK, outline=C_CYAN_ACC)

    # Right Face (Dark shade)
    right_poly = [(24, 28), (42, 18), (42, 33), (24, 43)]
    draw.polygon(right_poly, fill=(15, 90, 40, 255), outline=C_CYAN_ACC)

    # Glowing Neon Download Arrow on Top Face
    # Arrow head pointing down-forward
    arrow_pts = [
        (24, 13), (28, 17), (26, 17), (26, 21),
        (22, 21), (22, 17), (20, 17)
    ]
    draw.polygon(arrow_pts, fill=C_GOLD, outline=C_WHITE)

    # Seam lines and package details
    draw.line([(24, 28), (24, 43)], fill=C_LIME_HI, width=2)
    # Left diagonal stripe
    draw.line([(10, 23), (20, 38)], fill=C_LIME_MID, width=2)
    # Right diagonal stripe
    draw.line([(38, 23), (28, 38)], fill=C_LIME_MID, width=2)

    # "APKG" Micro-Glyphs on Left Face
    draw.line([(10, 27), (14, 27)], fill=C_WHITE, width=1)
    draw.line([(16, 29), (20, 29)], fill=C_WHITE, width=1)

    p48 = os.path.join(art_dir, "apkg_logo_48.png")
    img.save(p48)

    scaled = img.resize((192, 192), Image.NEAREST)
    scaled.save(os.path.join(art_dir, "apkg_logo_preview.png"))
    return p48

p_atlas = generate_atlas_logo()
p_apkg  = generate_apkg_logo()
print("Generated:", p_atlas, p_apkg)

