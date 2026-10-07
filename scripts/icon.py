# Draws OPM86 Player's icon, resources/icon.png: the YM2151 as a chip, with
# an FM wave and the SegaPCM's samples as bars on it, over the name in the
# app's pixel font. Needs Pillow; run from the repository root:
#   python3 scripts/icon.py
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter
S = 2048                      # drawn at 2x, saved at 1024
BG, PANEL, PANEL2 = (11,14,26), (20,26,48), (27,35,64)
CYAN, ORANGE, GRID = (61,224,255), (255,138,61), (57,67,111) # the app's theme colours
FONT = 'resources/fonts/PressStart2P-Regular.ttf'

def base():
    img = Image.new('RGBA', (S, S), (0,0,0,0))
    # The macOS grid: a rounded square 80% wide, with a soft shadow.
    m, r = int(S*0.1), int(S*0.18)
    sh = Image.new('RGBA', (S, S), (0,0,0,0)); ImageDraw.Draw(sh).rounded_rectangle((m, m+24, S-m, S-m+24), r, fill=(0,0,0,150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(30)))
    tile = Image.new('RGBA', (S, S), (0,0,0,0)); d = ImageDraw.Draw(tile)
    for y in range(m, S-m):  # a gentle vertical gradient
        t = (y-m)/(S-2*m); c = tuple(int(PANEL2[i]*(1-t)+BG[i]*t) for i in range(3))
        d.line((m, y, S-m, y), fill=c)
    mask = Image.new('L', (S, S), 0); ImageDraw.Draw(mask).rounded_rectangle((m, m, S-m, S-m), r, fill=255)
    img.paste(tile, (0,0), mask)
    return img, m, mask

def text(d, xy, s, size, fill, anchor='mm'):
    d.text(xy, s, font=ImageFont.truetype(FONT, size), fill=fill, anchor=anchor)

def glow(img, draw_fn, radius=28, strength=1):
    layer = Image.new('RGBA', img.size, (0,0,0,0)); draw_fn(ImageDraw.Draw(layer))
    for _ in range(strength): img.alpha_composite(layer.filter(ImageFilter.GaussianBlur(radius)))
    img.alpha_composite(layer)

def sine(d, x0, x1, y, amp, cycles, fill, width):
    pts = [(x, y - amp*math.sin((x-x0)/(x1-x0)*cycles*2*math.pi)) for x in range(int(x0), int(x1)+1, 4)]
    d.line(pts, fill=fill, width=width, joint='curve')

def chip():
    img, m, mask = base(); d = ImageDraw.Draw(img)
    cx = S//2; body = (cx-470, 560, cx+470, 1330)
    for i in range(8):  # pins, 8 a side: the YM2151's 8 channels
        y = 620 + i*90
        for x0, x1 in ((body[0]-110, body[0]), (body[2], body[2]+110)):
            d.rounded_rectangle((x0, y, x1, y+44), 10, fill=(150,160,200))
    d.rounded_rectangle(body, 40, fill=(16,20,38), outline=GRID, width=10)
    d.ellipse((body[0]+50, body[1]+50, body[0]+110, body[1]+110), fill=GRID)  # pin 1 mark
    glow(img, lambda g: sine(g, body[0]+90, body[2]-90, 820, 120, 2, CYAN, 34))
    d = ImageDraw.Draw(img)
    for i, h in enumerate([160, 260, 120, 300, 200, 90, 240, 150, 280, 110, 190]):  # PCM samples as bars
        x = body[0]+120 + i*70; d.rounded_rectangle((x, 1240-h, x+44, 1240), 8, fill=ORANGE)
    text(d, (cx, 1600), 'OPM86', 210, ORANGE)
    return img


chip().resize((1024, 1024), Image.LANCZOS).save('resources/icon.png')
