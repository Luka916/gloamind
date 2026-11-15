#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Render the mood sheet and the gaze-direction sheet straight from web/eyes.html.

Why screenshots and not hand-drawn art: the frames are computed, so anything drawn by hand
will disagree with the code sooner or later. This way the images cannot drift.

    python tests/gen_sheets.py

Requires Microsoft Edge or Chrome (headless) and Pillow. Writes:
    docs/images/moods-25.png
    docs/images/gaze-directions.png
"""
import os
import subprocess

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGE = os.path.join(ROOT, "web", "eyes.html")
OUT = os.path.join(ROOT, "docs", "images")
TMP = os.path.join(HERE, ".cells")
PROFILE = os.path.join(HERE, ".profile")

BROWSERS = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    "/usr/bin/google-chrome",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]

W, H = 560, 448          # the canvas is 40x32 cells of 14px
FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc", r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\Windows\Fonts\arial.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]

MOODS = [
    "normal", "happy", "smug", "despise", "blink",
    "wink", "peek", "surprise", "angry", "cry",
    "dizzy", "love", "speechless", "think", "sleep",
    "shy", "confused", "rainbow", "startle", "angry2",
    "nono", "nonoBlink", "dead", "nod", "glitch",
]

# Most moods have settled by frame 60. These carry their information in motion, so a fixed
# frame hides them; each number below is the frame that differs most from `normal`.
FRAME = {"nono": 30, "nonoBlink": 30, "nod": 30, "glitch": 90,
         "startle": 30, "angry2": 75, "think": 75}

# The page takes direction *names* (keys of DIRS), not numbers. Passing a number silently
# does nothing, which costs an hour if you do not know.
DIRS = [
    ("center", "centre"), ("up", "12"), ("upUpRight", "1"), ("upRight", "2"),
    ("right", "3"), ("downRight", "4"), ("downDownRight", "5"), ("down", "6"),
    ("downDownLeft", "7"), ("downLeft", "8"), ("left", "9"), ("upLeft", "10"),
    ("upUpLeft", "11"),
]


def browser():
    for p in BROWSERS:
        if os.path.exists(p):
            return p
    raise SystemExit("no headless browser found — edit BROWSERS at the top of this file")


def shot(exe, tag, query, path, frame=60):
    if os.path.exists(path) and os.path.getsize(path) > 800:
        return
    url = f"file:///{PAGE.replace(os.sep, '/')}?raw=1&still=1&f={frame}&{query}"
    subprocess.run([exe, "--headless=new", "--disable-gpu", "--no-first-run",
                    "--hide-scrollbars", "--force-device-scale-factor=1",
                    f"--user-data-dir={PROFILE}", f"--window-size={W},{H}",
                    f"--screenshot={path}", url], capture_output=True)
    if not os.path.exists(path):
        print(f"  !! failed: {tag}")


def font(size):
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def compose(items, cols, cw, ch, label_h, out_path, title):
    pad, title_h = 8, 46
    rows = (len(items) + cols - 1) // cols
    im = Image.new("RGB", (cols * cw + (cols + 1) * pad,
                           title_h + rows * (ch + label_h) + (rows + 1) * pad), "#07090a")
    d = ImageDraw.Draw(im)
    f_lab, f_tit = font(17), font(26)
    d.text((pad + 4, 12), title, font=f_tit, fill="#c8d8dc")

    for i, (label, path) in enumerate(items):
        r, c = divmod(i, cols)
        x = pad + c * (cw + pad)
        y = title_h + pad + r * (ch + label_h + pad)
        if os.path.exists(path):
            im.paste(Image.open(path).convert("RGB").resize((cw, ch), Image.LANCZOS), (x, y))
        d.rectangle([x, y, x + cw - 1, y + ch - 1], outline="#1c2427")
        tw = d.textlength(label, font=f_lab)
        d.text((x + (cw - tw) / 2, y + ch + 5), label, font=f_lab, fill="#8fa3a8")

    im.save(out_path, optimize=True)
    print(f"  -> {os.path.relpath(out_path, ROOT)}  {im.size[0]}x{im.size[1]}")


def main():
    exe = browser()
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)

    print("moods…")
    moods = []
    for m in MOODS:
        p = os.path.join(TMP, f"mood_{m}.png")
        shot(exe, m, f"m={m}", p, FRAME.get(m, 60))
        moods.append((m, p))

    print("directions…")
    dirs = []
    for key, lab in DIRS:
        p = os.path.join(TMP, f"dir_{key}.png")
        shot(exe, key, f"d={key}", p)
        dirs.append((lab if key == "center" else f"{lab} o'clock", p))

    print("composing…")
    compose(moods, 5, 224, 179, 26, os.path.join(OUT, "moods-25.png"),
            "25 expressions — every frame computed, nothing played back")
    compose(dirs, 5, 224, 179, 26, os.path.join(OUT, "gaze-directions.png"),
            "13 gaze directions — a clock face, centre included")


if __name__ == "__main__":
    main()
