#!/usr/bin/env python3
"""AMGR: one place that defines the parameters and the pages. Writes params.json, layout.conf and the two C headers the engine and the screen share
(src/amgr_idx.h, src/amgr_marks.h). Run from the project folder: python3 tools/gen_amgr.py"""
import json, os
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
L = 4
P = []   # (key, name, min, max, def, kind, extra dict)
def add(key, name, lo, hi, df, kind, **x): P.append((key, name, lo, hi, df, kind, x))

# first, so a project reload sets them before the sound parameters (a preset must never overwrite what the project restores after it)
add("preset", "FACTORY", 0, 255, 0, "FACT", display="int", nudge_pct=10, dynamic_display=True)
add("user", "USER", 1, 999, 1, "USER", display="int", nudge_pct=10, dynamic_display=True)
add("save", "SAVE", 0, 1, 0, "SAVE", momentary=True, dynamic_display=True)
add("bend", "BEND", 1, 24, 2, "BEND", display="int", dynamic_display=True)
for i in range(L): add("wav%d" % (i + 1), "WAV %d" % (i + 1), 0, 999, 0, "WAV", display="int", nudge_pct=5, dynamic_display=True)
add("envl", "LAYER", 1, 4, 1, "ENVL", display="int", dynamic_display=True)
# the four ENVELOPE knobs: they show and move the selected layer's own values (atk1..rel4 below)
add("atk", "ATTACK", 0, 1, 0.15, "ENV", dynamic_display=True)
add("dec", "DECAY", 0, 1, 0.45, "ENV", dynamic_display=True)
add("sus", "SUSTAIN", 0, 1, 0.8, "PCT", dynamic_display=True)
add("rel", "RELEASE", 0, 1, 0.45, "REL", dynamic_display=True)
posd = [0.10, 0.30, 0.55, 0.80]; pand = [0.30, 0.70, 0.15, 0.85]; lvld = [0.80, 0.60, 0.50, 0.40]
for i in range(L): add("pos%d" % (i + 1), "POS %d" % (i + 1), 0, 1, posd[i], "POS", dynamic_display=True)
for i in range(L): add("size%d" % (i + 1), "SIZE %d" % (i + 1), 0, 1, 0.45, "SIZE", dynamic_display=True)
for i in range(L): add("dens%d" % (i + 1), "DENS %d" % (i + 1), 0, 1, 0.6, "DENS", dynamic_display=True)
for i in range(L): add("pitch%d" % (i + 1), "PITCH %d" % (i + 1), -24, 24, 0, "ST", display="int", dynamic_display=True)
for i in range(L): add("level%d" % (i + 1), "LEVEL %d" % (i + 1), 0, 1, lvld[i], "PCT", dynamic_display=True)
for i in range(L): add("sprd%d" % (i + 1), "SPRD %d" % (i + 1), 0, 1, 0.1, "PCT", dynamic_display=True)
for i in range(L): add("pan%d" % (i + 1), "PAN %d" % (i + 1), 0, 1, pand[i], "PAN", dynamic_display=True)
for i in range(L): add("atk%d" % (i + 1), "ATK %d" % (i + 1), 0, 1, 0.15, "ENV", dynamic_display=True)
for i in range(L): add("dec%d" % (i + 1), "DEC %d" % (i + 1), 0, 1, 0.45, "ENV", dynamic_display=True)
for i in range(L): add("sus%d" % (i + 1), "SUS %d" % (i + 1), 0, 1, 0.8, "PCT", dynamic_display=True)
for i in range(L): add("rel%d" % (i + 1), "REL %d" % (i + 1), 0, 1, 0.45, "REL", dynamic_display=True)
add("cut", "CUTOFF", 0, 1, 1.0, "CUT", dynamic_display=True)
add("res", "RES", 0, 1, 0.15, "PCT", dynamic_display=True)
add("ftype", "FILTER", 0, 1, 0.0, "FTYPE", dynamic_display=True)
add("drive", "DRIVE", 0, 1, 0.0, "PCT", dynamic_display=True)
add("vol", "VOLUME", 0, 1, 0.8, "PCT", dynamic_display=True)
add("shape", "SHAPE", 0, 1, 0.5, "PCT", dynamic_display=True)
add("jitt", "JITTER", 0, 1, 0.3, "PCT", dynamic_display=True)
add("rev", "REVERSE", 0, 1, 0.0, "PCT", dynamic_display=True)
add("dtime", "TIME", 0, 1, 0.5, "DTIME", dynamic_display=True)
add("dfb", "FDBK", 0, 1, 0.35, "PCT", dynamic_display=True)
add("dtone", "TONE", 0, 1, 0.6, "PCT", dynamic_display=True)
add("dmix", "DLY MIX", 0, 1, 0.0, "PCT", dynamic_display=True)
add("vsize", "VB SIZE", 0, 1, 0.6, "PCT", dynamic_display=True)
add("vdamp", "VB DAMP", 0, 1, 0.4, "PCT", dynamic_display=True)
add("vmix", "VB MIX", 0, 1, 0.15, "PCT", dynamic_display=True)
add("width", "WIDTH", 0, 1, 0.8, "PCT", dynamic_display=True)

keys = [p[0] for p in P]
def ks(prefix): return [prefix + str(i + 1) for i in range(L)]
TABS = [("WAVES", ks("pos")), ("WINDOW", ks("size")), ("DENSITY", ks("dens")), ("PITCH", ks("pitch")), ("LEVEL", ks("level")),
        ("SPREAD", ks("sprd")), ("PAN", ks("pan")), ("ENVELOPE", ["atk", "dec", "sus", "rel"]), ("FILTER", ["cut", "res", "ftype", "drive"]),
        ("MAIN", ["vol", "shape", "jitt", "rev"]), ("DELAY", ["dtime", "dfb", "dtone", "dmix"]), ("REVERB", ["vsize", "vdamp", "vmix", "width"]),
        ("SAMPLES", []), ("PRESETS", [])]
# what the Q-Links 1..8 hold on each tab: the four controls sit on the middle four (3..6)
QL = {tn: ["-", "-"] + kk + ["-", "-"] for tn, kk in TABS}
QL["ENVELOPE"] = ["envl", "-", "atk", "dec", "sus", "rel", "-", "-"]
QL["SAMPLES"] = ["-", "-"] + ks("wav") + ["-", "-"]
QL["PRESETS"] = ["-", "-", "preset", "user", "bend", "-", "-", "-"]
name = {p[0]: p[1] for p in P}

params = []
for key, nm, lo, hi, df, kind, x in P:
    d = {"key": key, "name": nm, "min": lo, "max": hi, "default": df}; d.update(x); params.append(d)
json.dump({"name": "AMGR", "params": params}, open(os.path.join(root, "params.json"), "w"), indent=1)

with open(os.path.join(root, "layout.conf"), "w") as f:
    f.write("theme_bg=000000\ntheme_panel=000000\ntheme_ink=f0f0f0\ntheme_accent=c96a3c\nlabel_scale=1.7\n")
    for tn, kk in TABS:
        f.write("\n[tab %s]\n" % tn)
        for j, k in enumerate(kk):
            f.write('knob cx=%d cy=536 r=59 label="%s" key=%s\n' % (235 + 280 * j, name[k], k))
        f.write('qlinks "%s" = %s\n' % (tn, ",".join(QL[tn])))

with open(os.path.join(root, "src", "amgr_idx.h"), "w") as f:
    f.write("/* generated by tools/gen_amgr.py: parameter indices (the order of params.json) and how each one is shown */\n#pragma once\n")
    kinds = sorted(set(p[5] for p in P))
    f.write("enum { %s };\n" % ", ".join("K_%s" % k for k in kinds))
    f.write("enum {\n")
    for i, k in enumerate(keys): f.write("    P_%s = %d,\n" % (k.upper(), i))
    f.write("    NP = %d\n};\n" % len(P))
    f.write("static const unsigned char AM_KIND[NP] = {%s};\n" % ", ".join("K_" + p[5] for p in P))
    f.write("#define GR_PAGES_N %d\n" % len(TABS))
    for i, (tn, _) in enumerate(TABS): f.write("#define PG_%s %d\n" % (tn, i))

with open(os.path.join(root, "src", "amgr_marks.h"), "w") as f:
    f.write("/* generated by tools/gen_amgr.py: the page marker colours, id n -> base 91 + 12 n (ids 0 and 1 are the proven ones) */\n#pragma once\n")
    tl, br = [], []
    for n in range(len(TABS)):
        B = 91 + 12 * n
        tl.append("{{23, 7, %d}, {%d, 7, 23}, {7, %d, 23}, {23, %d, 7}}" % (B, B, B, B))
        br.append("{{%d, 23, 7}, {7, 23, %d}, {%d, 7, 61}, {7, %d, 61}}" % (B, B, B - 30, B - 30))
    f.write("static const unsigned char GR_MARK_TL[%d][4][3] = {\n    %s,\n};\n" % (len(TABS), ",\n    ".join(tl)))
    f.write("static const unsigned char GR_MARK_BR[%d][4][3] = {\n    %s,\n};\n" % (len(TABS), ",\n    ".join(br)))
print("params", len(P), "tabs", len(TABS))
