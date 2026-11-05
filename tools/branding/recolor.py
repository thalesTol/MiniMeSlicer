#!/usr/bin/env python3
"""MiniMe brand recolour: swap Bambu Studio's brand green for MiniMe blue.

Run from anywhere:  python tools/branding/recolor.py [--dry-run] [--include-locked]

The sweep is idempotent: no target colour is also a source colour, so a second
run changes nothing. Standard library only.

Palette (Bambu -> MiniMe)
  #00AE42 (0,174,66)  main brand green, + #00AF42 / (0,177,66)   -> #1E6EF0 (30,110,240)
  #3DCB73 (61,203,115) hover                                     -> #4A8DF6 (74,141,246)
  #1B8844 (27,136,68)  pressed                                   -> #1656C8 (22,86,200)
  #30DD70 (48,221,112) light hover                               -> #5B9DFF (91,157,255)
  #17813F (23,129,63)  dark pressed                              -> #1450B8 (20,80,184)
  #DBFDE7 / #DBFDD5    light tints                               -> #DDEAFF (221,234,255)
  (0,190,74) hover, (0,240,91) link hover                        -> #3A84F5, #5B9DFF
  ImVec4(0.00f,0.68f,0.26f,a)                                    -> ImVec4(0.118f,0.431f,0.941f,a)
  COL_GREEN_LIGHT (0.86,0.99,0.91)                               -> (0.867f,0.918f,1.0f)
  x/255 float forms of (0,174,66)                                -> (30,110,240)/255
  Dark-mode partners: #21A452 -> #2E78E8, #1C8A46 -> #1A5CC0, #37B865 -> #4A8BEE,
  #1F3529 -> #1E2E4A, ImGui dark tint (43,64,54)/255 -> (30,46,74)/255.
  Plus a few near-duplicate brand hover/press greens used by the web pages and
  icons (see HEX_MAP), and the selected-row light green tints (#EBF9F0, #EDFAF2).

Integer colours in C++ need care:
  * StateColor's append(unsigned long)/make_pair(0xRRGGBB, ...) reads 0xRRGGBB, so
    0x00AE42 there becomes 0x1E6EF0.
  * wxColour(unsigned long) (std::pair<wxColour,int>(0x..), SetForegroundColour(0x..),
    dc.SetTextForeground(0x..)) reads 0xBBGGRR, so those become wxColour(30, 110, 240).
  * 0xFF42AE00 (ImGui ABGR) becomes 0xFFF06E1E.

Kept green on purpose (real filament colours, status colours, third-party code):
  see EXCLUDE_* and PROTECT below.

Done by hand, outside this script:
  * StateColor.cpp gDarkColors: #DBFDE7 and #DBFDD5 both become #DDEAFF, so the
    duplicate key was removed by hand (kept #DDEAFF -> #1E2E4A).
  * BBLTopbar.cpp: navy title bar (#0F1B33) with its own hover/pressed shades.
  * Comments in this sweep's targets must not spell old colours with '#'.
"""

import argparse
import fnmatch
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# --------------------------------------------------------------------------
# Palette
# --------------------------------------------------------------------------
HEX_MAP = {
    # main
    "00AE42": "1E6EF0", "00AF42": "1E6EF0", "00B142": "1E6EF0",
    "06AE42": "1E6EF0", "00B12C": "1E6EF0", "00B731": "1E6EF0", "50A90B": "1E6EF0",
    # hover / lighter
    "3DCB73": "4A8DF6", "73CA94": "4A8DF6",
    "00BE4A": "3A84F5", "00B35C": "3A84F5", "00C74E": "3A84F5",
    "30DD70": "5B9DFF", "37EE7C": "5B9DFF", "00F05B": "5B9DFF",
    # pressed / darker
    "1B8844": "1656C8", "009638": "1656C8", "009438": "1656C8",
    "009A3A": "1656C8", "00A03D": "1656C8",
    "17813F": "1450B8",
    # light tints
    "DBFDE7": "DDEAFF", "DBFDD5": "DDEAFF", "CAFCD5": "DDEAFF",
    "EBF9F0": "EBF2FE", "EDFAF2": "EDF3FE",
    "E0FCEB": "DDEAFF", "E8F5E9": "EAF1FE", "E6F7ED": "EAF1FE", "DEF5E7": "DDEAFF",
    "D7E8DE": "D5E2F7",
    # dark-mode partners
    "21A452": "2E78E8", "4CAA50": "2E78E8",
    "1C8A46": "1A5CC0", "37B865": "4A8BEE",
    "1F3529": "1E2E4A", "243E30": "1E2E4A", "293F34": "233350",
    "1F2B27": "1F2A3D",
}

RGB_MAP = {
    (0, 174, 66): (30, 110, 240),
    (0, 177, 66): (30, 110, 240),
    (61, 203, 115): (74, 141, 246),
    (27, 136, 68): (22, 86, 200),
    (48, 221, 112): (91, 157, 255),
    (23, 129, 63): (20, 80, 184),
    (219, 253, 231): (221, 234, 255),
    (219, 253, 213): (221, 234, 255),
    (0, 190, 74): (58, 132, 245),
    (0, 240, 91): (91, 157, 255),
}

# Near-duplicate brand accents found in single dialogs / ImGui overlays.
# C++ only: in the compiled web bundles the same triples are Tailwind class
# names whose escaped CSS selectors would no longer match.
RGB_MAP_CPP = {
    (8, 153, 46): (30, 110, 240),    # MsgDialog link
    (0, 174, 100): (30, 110, 240),   # ObjColor / SyncAmsInfo tips
    (25, 166, 77): (30, 110, 240),   # assembly guide selected
    (44, 173, 0): (30, 110, 240),    # assembly guide slot highlight (alpha 38)
    (0, 200, 80): (58, 132, 245),    # assembly guide drag handle
    (0, 150, 57): (22, 86, 200),     # wiki link hover
    (0, 150, 50): (22, 86, 200),     # Helio button hover
    (30, 60, 40): (30, 46, 74),      # dark-mode selected option background
}

# 0xRRGGBB ints used through StateColor (RGB order).
INT_RGB_MAP = {"00AE42": "1E6EF0", "3DCB73": "4A8DF6", "1B8844": "1656C8"}

# --------------------------------------------------------------------------
# Where to look
# --------------------------------------------------------------------------
AREAS = {
    "cpp": {
        "roots": ["src/slic3r", "src/imgui"],
        "ext": (".cpp", ".hpp", ".h"),
    },
    "svg": {
        "roots": ["resources/images"],
        "ext": (".svg",),
    },
    "web": {
        "roots": ["resources/web", "src/slic3r/GUI/DeviceWeb/device_page"],
        "ext": (".css", ".html", ".htm", ".js", ".ts", ".tsx", ".svg"),
    },
}

EXCLUDE_DIRS = {"node_modules", ".git", "build", "deps"}

# Glob patterns (forward slashes, relative to repo root) that are never touched.
EXCLUDE_GLOBS = [
    # real filament colours
    "src/libslic3r/*",
    "resources/profiles/*",
    "resources/flush/*",
    # third-party / generated
    "*.map",
    "*.min.js", "*.min.css",
    "resources/web/include/*",
    "src/imgui/imgui.cpp", "src/imgui/imgui_draw.cpp", "src/imgui/imgui_tables.cpp",
    # icons that show filament, hardware or status colours rather than UI accent
    "resources/images/ams_humidity_*.svg",     # humidity status (green = dry)
    "resources/images/pipe_of_*.svg",          # filament flowing in the AMS pipe
    "resources/images/filament_green.svg",     # literally a green filament
    "resources/images/flag_green.svg",         # preset "compatible" status flag
    "resources/images/flag_red.svg",           # preset "incompatible" status flag
    "resources/images/printer_thumbnail*.svg", # printer hardware renders
    # src/web: mock tray data is filament colour (handled by PROTECT where mixed)
]

# Files another change owns. They are scanned and reported, never edited,
# unless --include-locked is passed.
LOCKED_GLOBS = [
    # made by make_logo.py / render_logo.ps1, or MiniMe's own blue already
    "*MiniMeChat*",
    "resources/images/BambuStudio*",
    "resources/images/splash_logo.svg",
    "resources/web/minime_chat/*",
    "tools/branding/make_logo*",
]

# Spans inside a file that must stay as they are (filament colour data mixed
# with UI code). Matches inside these spans are skipped.
PROTECT = {
    # filament palettes, gradients, translucent swatches and the default
    # filament colour are all quoted hex literals; UI accents use wxColour(r,g,b)
    "src/slic3r/GUI/FilamentLookDialog.cpp": [r'"#[0-9A-Fa-f]{6}(?:[0-9A-Fa-f]{2})?"'],
    # AMS mock tray: tray('0','0','A1','#00AE42','PLA', ...)
    "src/slic3r/GUI/DeviceWeb/device_page/src/features/device-page/ams-control-web/mockData.ts":
        [r"tray\([^)]*\)"],
    "*/device_page/dist/assets/index.js":
        [r'\w+\("\d+","\d+","[A-Z]\d+","#[0-9A-Fa-f]{6,8}","[^"]*"'],
}

# --------------------------------------------------------------------------
# Rules
# --------------------------------------------------------------------------
HEX_ALT = "|".join(sorted(HEX_MAP, key=len, reverse=True))
# '#' or URL-encoded '%23', then the colour, optionally followed by 2 alpha digits
RE_HEX = re.compile(r"(#|%23)(" + HEX_ALT + r")(?=(?:[0-9A-Fa-f]{2})?(?![0-9A-Fa-f]))", re.I)


def _case_like(src, new):
    return new.lower() if src == src.lower() and src != src.upper() else new.upper()


def sub_hex(m):
    src = m.group(2)
    return m.group(1) + _case_like(src, HEX_MAP[src.upper()])


def _triple_re(rgb):
    r, g, b = rgb
    # "(" "{" or "rgb(" / "rgba(" opener; must be followed by "," ")" or "}"
    return re.compile(r"([({]\s*)%d(\s*,\s*)%d(\s*,\s*)%d(?=\s*[,)}])" % (r, g, b))


RE_TRIPLES = [(_triple_re(k), v) for k, v in RGB_MAP.items()]
RE_TRIPLES_CPP = [(_triple_re(k), v) for k, v in RGB_MAP_CPP.items()]

# number with optional decimals / f suffix, optionally divided by 255
_DIV = r"(?:\s*/\s*255(?:\.\d*)?f?)"
def _comp(n):
    return r"%d(?:\.0*)?f?%s?" % (n, _DIV)

RE_DIV_BRAND = re.compile(r"([({]\s*)" + _comp(0) + r"\s*,\s*" + _comp(174) + r"\s*,\s*" + _comp(66) + r"(?=\s*[,)}])")
RE_DIV_DARKTINT = re.compile(r"\b43(\s*/\s*255\.0f?\s*,\s*)64(\s*/\s*255\.0f?\s*,\s*)54(\s*/\s*255\.0f?)")

CPP_RULES = [
    # wxColour(unsigned long) contexts read 0xBBGGRR: write the colour explicitly.
    ("wxcolour-int", re.compile(r"(std::pair<wxColour,\s*int>\(\s*)0x00AE42\b", re.I), r"\1wxColour(30, 110, 240)"),
    ("wxcolour-int", re.compile(r"\b(SetForegroundColour|SetTextForeground|SetBackgroundColour)\(\s*0x00AE42\s*\)", re.I),
     r"\1(wxColour(30, 110, 240))"),
    ("hex", re.compile(r'"#0x00AE42"', re.I), '"#1E6EF0"'),
    # StateColor ints read 0xRRGGBB.
    ("stateclr-int", re.compile(r"\b0x(" + "|".join(INT_RGB_MAP) + r")\b", re.I),
     lambda m: "0x" + _case_like(m.group(1), INT_RGB_MAP[m.group(1).upper()])),
    # ImGui ABGR u32
    ("imgui-abgr", re.compile(r"\b0xFF42AE00\b", re.I), "0xFFF06E1E"),
    # GLGizmoMove hover colour written in (wrong) b,g,r order
    ("float", re.compile(r"\{66\.f / 255\.f, 174\.f / 255\.f, 0, 1\}"), "{30.f / 255.f, 110.f / 255.f, 240.f / 255.f, 1}"),
    # floats
    ("float", re.compile(r"([({]\s*)0(?:\.0*)?f?(\s*,\s*)0\.68f?(\s*,\s*)0\.26f?(?=\s*[,)}])"), r"\g<1>0.118f\g<2>0.431f\g<3>0.941f"),
    ("float", re.compile(r"([({]\s*)0(?:\.0*)?f?(\s*,\s*)0\.682f?(\s*,\s*)0\.259f?(?=\s*[,)}])"), r"\g<1>0.118f\g<2>0.431f\g<3>0.941f"),
    ("float", re.compile(r"([({]\s*)0\.86f?(\s*,\s*)0\.99f?(\s*,\s*)0\.91f?(?=\s*[,)}])"), r"\g<1>0.867f\g<2>0.918f\g<3>1.0f"),
    ("float", RE_DIV_BRAND, lambda m: (m.group(1) + "30.0f / 255.0f, 110.0f / 255.0f, 240.0f / 255.0f") if "/" in m.group(0) else m.group(0)),
    ("float", RE_DIV_DARKTINT, r"30\g<1>46\g<2>74\g<3>"),
]


# --------------------------------------------------------------------------
def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def matches_any(relpath, globs):
    return any(fnmatch.fnmatch(relpath, g) for g in globs)


def protected_spans(relpath, text):
    spans = []
    for pattern_glob, regexes in PROTECT.items():
        if fnmatch.fnmatch(relpath, pattern_glob):
            for rx in regexes:
                spans += [m.span() for m in re.finditer(rx, text)]
    return spans


def apply(rx, repl, text, spans, counter, key):
    def _sub(m):
        if any(a <= m.start() < b for a, b in spans):
            return m.group(0)
        out = repl(m) if callable(repl) else m.expand(repl)
        if out != m.group(0):
            counter[key] = counter.get(key, 0) + 1
        return out
    return rx.sub(_sub, text)


def recolor_text(relpath, text, area):
    counter = {}
    spans = protected_spans(relpath, text)
    if area == "cpp":
        for key, rx, repl in CPP_RULES:
            text = apply(rx, repl, text, spans, counter, key)
            spans = protected_spans(relpath, text)
    for rx, (r, g, b) in RE_TRIPLES + (RE_TRIPLES_CPP if area == "cpp" else []):
        text = apply(rx, lambda m, r=r, g=g, b=b: "%s%d%s%d%s%d" % (m.group(1), r, m.group(2), g, m.group(3), b),
                     text, spans, counter, "rgb")
        spans = protected_spans(relpath, text)
    text = apply(RE_HEX, sub_hex, text, spans, counter, "hex")
    return text, counter


def iter_files(area):
    cfg = AREAS[area]
    for root in cfg["roots"]:
        base = os.path.join(ROOT, root)
        for dp, dns, fns in os.walk(base):
            dns[:] = [d for d in dns if d not in EXCLUDE_DIRS]
            for fn in fns:
                if fn.lower().endswith(cfg["ext"]):
                    yield os.path.join(dp, fn)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="report only, write nothing")
    ap.add_argument("--include-locked", action="store_true", help="also edit files in LOCKED_GLOBS")
    args = ap.parse_args()

    seen = set()
    totals = {}
    locked_hits = []
    for area in AREAS:
        nfiles = 0
        for path in iter_files(area):
            r = rel(path)
            if r in seen or matches_any(r, EXCLUDE_GLOBS):
                continue
            seen.add(r)
            with open(path, "rb") as f:
                raw = f.read()
            text = raw.decode("utf-8", "surrogateescape")
            new, counter = recolor_text(r, text, "cpp" if path.endswith((".cpp", ".hpp", ".h")) else area)
            if new == text:
                continue
            if matches_any(r, LOCKED_GLOBS) and not args.include_locked:
                locked_hits.append((r, counter))
                continue
            nfiles += 1
            print("  %-90s %s" % (r, ", ".join("%s=%d" % kv for kv in sorted(counter.items()))))
            if not args.dry_run:
                with open(path, "wb") as f:
                    f.write(new.encode("utf-8", "surrogateescape"))
        totals[area] = nfiles
    print()
    for area, n in totals.items():
        print("%-4s files %s: %d" % (area, "to change" if args.dry_run else "changed", n))
    if locked_hits:
        print("\nLocked files with brand green (not edited):")
        for r, c in locked_hits:
            print("  %-90s %s" % (r, ", ".join("%s=%d" % kv for kv in sorted(c.items()))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
