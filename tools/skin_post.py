#!/usr/bin/env python3
"""AMGR skin touch-ups, run by gen_vst after the skin is built (python3 skin_post.py <skin dir>).
1. Knobs: reMixXXL's own knob art (knob.png turned in place, 101 frames: the same knobs that turn in place on the Force) at the
   Meta4MPCXXL text sizes (name 35.7 px, value 37.4 px, 221 x 188 cell), each placed where layout.conf puts it.
2. Page markers: a 2 x 2 colour block in the top-left and bottom-right corner of the page background of every tab, so the live waveform
   screen (src/gr2_overlay.c) can find the page on the display and tell which tab is shown (XYZ's / reMixXXL's recipe)."""
import json, os, re, sys
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
top = sys.argv[1]
skin = os.path.join(top, "Plugin Skins")
DIA, CELL_W, CELL_H = 100, 221, 190

# ---- positions from layout.conf: label -> (cx, cy)
pos = {}
for line in open(os.path.join(here, "..", "layout.conf")):
    m = re.match(r'\s*knob\s+cx=(\d+)\s+cy=(\d+).*label="([^"]+)"', line)
    if m: pos[m.group(3)] = (int(m.group(1)), int(m.group(2)))

# ---- 1. the knob art: VARIANT's own knob filmstrip (64 frames of 126 x 126, turns in place), its flat background made transparent, scaled to DIA
import collections
import numpy as np
vk = Image.open(os.path.join(here, "..", "third_party", "art", "variant_knob.png")).convert("RGB")
FR_SRC = vk.width
FRAMES = vk.height // FR_SRC
strip = Image.new("RGBA", (DIA, DIA * FRAMES), (0, 0, 0, 0))
for k in range(FRAMES):
    fr = vk.crop((0, k * FR_SRC, FR_SRC, (k + 1) * FR_SRC)).convert("RGBA")
    ref = fr.getpixel((0, 0)); px = fr.load(); W_, H_ = fr.size
    seen = set(); q = collections.deque([(0, 0), (W_ - 1, 0), (0, H_ - 1), (W_ - 1, H_ - 1)]); seen.update(q)
    while q:
        x, y = q.popleft(); c = px[x, y]
        if sum(abs(c[i] - ref[i]) for i in range(3)) > 14: continue
        px[x, y] = (0, 0, 0, 0)
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < W_ and 0 <= ny < H_ and (nx, ny) not in seen: seen.add((nx, ny)); q.append((nx, ny))
    fr = fr.resize((DIA, DIA), Image.LANCZOS)
    # a little brighter black: the dark body lifted by a gamma, plus a soft light from the top that stays put while the knob turns (alpha untouched)
    a_ = np.asarray(fr).astype(np.float32); rgb_ = a_[..., :3] / 255.0
    rgb_ = np.power(rgb_, 0.78)
    rgb_ += (0.075 * (1.0 - np.linspace(0, 1, DIA, dtype=np.float32))[:, None, None]) * (1.0 - rgb_)
    a_[..., :3] = np.clip(rgb_ * 255.0, 0, 255)
    strip.alpha_composite(Image.fromarray(a_.astype(np.uint8), "RGBA"), (0, k * DIA))
assert strip.height <= 12288, strip.height
old = os.path.join(skin, "sh_knob_r59.png")
if os.path.exists(old): os.remove(old)
strip.save(os.path.join(skin, "rx_knob_92.png"))
p = os.path.join(skin, "TUI.json"); d = json.load(open(p))
cd = d["pageData"]["componentDefinitions"]["localComponentDefinitions"]
parts = cd[0]["value"]["componentsData"]
def bounds(c, b): c["bounds"]["bounds"] = b
bounds(parts[0], "0 0 %d %d" % (CELL_W, CELL_H))
k = parts[1]["componentData"]["data"]; k["filmStrip"] = "rx_knob_92.png"; k["numFrames"] = FRAMES
bounds(parts[1], "%d 0 %d %d" % ((CELL_W - DIA) // 2, DIA, DIA))
parts[2]["componentData"]["data"]["textStyle"]["font"].update(height=35.7, style="Regular"); bounds(parts[2], "0 102 %d 46" % CELL_W)
parts[3]["componentData"]["data"]["textStyle"]["font"].update(height=37.4, style="Regular"); bounds(parts[3], "0 148 %d 42" % CELL_W)
placed = 0
for kdef in cd[1:]:
    for c in kdef["value"]["componentsData"]:
        n = c["componentData"]["name"]
        if n in pos:
            cx, cy = pos[n]; bounds(c, "%d %d %d %d" % (cx - CELL_W // 2, cy - 100, CELL_W, CELL_H)); placed += 1
assert placed == len(pos), (placed, len(pos))
json.dump(d, open(p, "w"), indent=2)

# ---- 1b. invisible controls (a Focus only, see-through) over the sample tiles and the preset tile: they take MPC's focus when tapped, so the
# data wheel turns their parameter (the live screen draws the tiles; the wrapper routes the wheel to the open list)
import copy
pj = json.load(open(os.path.join(here, "..", "params.json")))["params"]
pidx = {q["key"]: i for i, q in enumerate(pj)}
def hit_def(key, w, h):
    dd = copy.deepcopy(cd[0]); dd["key"] = key
    dd["value"]["actions"] = [a for a in dd["value"]["actions"] if a["onAction"] == "Mouse Down"]
    foc = dd["value"]["componentsData"][0]; bounds(foc, "0 0 %d %d" % (w, h))
    dd["value"]["componentsData"] = [foc]
    return dd
cd.append(hit_def("shHit292", 292, 84)); cd.append(hit_def("shHit780", 780, 84))
def place_hit(page_key, name, typ, pkey, x, y, w, h):
    for kdef in cd:
        if kdef["key"] == page_key:
            c = copy.deepcopy(next(cc for kk in cd[1:] for cc in kk["value"]["componentsData"] if cc["componentData"]["type"] == "shKnob59_ls1.7"))
            c["componentData"]["name"] = name; c["componentData"]["type"] = typ
            c["handle remapping"]["map"] = [{"key": "Data", "value": "Parameter %d" % pidx[pkey]}]
            bounds(c, "%d %d %d %d" % (x, y, w, h))
            kdef["value"]["componentsData"].append(c); return
    raise SystemExit("page %s not found" % page_key)
for L in range(4): place_hit("SAMPLES|SAMPLES", "WAV %d" % (L + 1), "shHit292", "wav%d" % (L + 1), 22 + L * 310, 482, 292, 84)
place_hit("PRESETS|PRESETS", "PRESET", "shHit780", "preset", 22, 482, 780, 84)
json.dump(d, open(p, "w"), indent=2)

# ---- 2. the page markers: page id n (the tab's order) -> base colour 91 + 12 n, the same tables src/amgr_marks.h holds (tools/gen_amgr.py)
def marks(n):
    B = 91 + 12 * n
    return [(23, 7, B), (B, 7, 23), (7, B, 23), (23, B, 7)], [(B, 23, 7), (7, 23, B), (B - 30, 7, 61), (7, B - 30, 61)]
pages = 0
while os.path.exists(os.path.join(skin, "sh_bg_%d.png" % pages)): pages += 1
assert pages == int(os.environ.get("AMGR_PAGES", pages)) and pages > 0, pages
for page in range(pages):
    f = os.path.join(skin, "sh_bg_%d.png" % page)
    im = Image.open(f).convert("RGB"); w, h = im.size
    assert (w, h) == (1280, 628), (w, h)
    from PIL import ImageDraw
    dr = ImageDraw.Draw(im); dr.rectangle((0, 0, w, 425), fill=(30, 30, 28)); dr.rectangle((0, 426, w, h), fill=(0, 0, 0))
    px = im.load()
    tl, br = marks(page)
    for kk in range(4):
        px[(kk & 1), (kk >> 1)] = tl[kk]
        px[w - 2 + (kk & 1), h - 2 + (kk >> 1)] = br[kk]
    im.save(f)
print("AMGR skin: %d knobs placed, markers on %d pages" % (placed, pages))
