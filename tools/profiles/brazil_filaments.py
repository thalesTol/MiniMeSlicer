"""MiniMe Slicer: filament profiles for Brazilian brands, for every Bambu Lab printer.

Each line is built from MiniMe's matching Generic profile (same printer-specific tuning: fans, flow, plate temperatures per
printer) and changes only what the manufacturer states: nozzle temperature and range, bed temperature, density and the
maximum flow (never raised above the Generic value). The filament_id stays the Generic one, so Bambu printers and the AMS
treat the spool as that generic material.

Sources (manufacturer pages, November 2026):
  STLFLIX      https://stlflix.com.br  (PLA, PLA Matte, PLA Silk; PETG page gives no temperatures)
  Voolt3D      https://voolt3d.com.br  (PLA High Speed Premium, PLA Velvet, PLA V-Silk, PETG HF)
  GTMax3D      https://www.gtmax3d.com.br  (PLA Speed+, PETG HF; ABS Speed+ page gives no temperatures)
  National3D   https://www.sethi3d.com.br  (PLA Max High Speed, ABS Premium MG-94)
  Masterprint  product data as sold by KaBuM  (PLA)

Usage: python tools/profiles/brazil_filaments.py   (writes resources/profiles/BBL/filament/*.json, updates BBL.json;
running it again replaces what it made before)
"""

import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
PROFILES = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "profiles"))
FILAMENT_DIR = os.path.join(PROFILES, "BBL", "filament")
BBL_JSON = os.path.join(PROFILES, "BBL.json")
# raised so MiniMe copies the new profiles into its data folder (system/BBL) at the next start
BUNDLE_VERSION = "02.08.00.90"

# name, vendor, generic family, manufacturer data (None = keep the Generic value)
LINES = [
    ("STLFLIX PLA", "STLFLIX", "Generic PLA",
     dict(nozzle=215, first=220, low=195, high=230, bed=60, density=1.24,
          note="195-220 C on standard printers, 200-230 C on fast printers (Bambu Lab), bed 60 C",
          # calibrated on the user's A1 (Bambu Studio flow rate calibration, 0.4 nozzle) at 220 C, bed 65 C
          calibrated={"@BBL A1": dict(flow=0.9702, nozzle=220, first=220, bed=65)})),
    ("STLFLIX PLA Matte", "STLFLIX", "Generic PLA",
     dict(nozzle=215, first=220, low=195, high=230, bed=60, density=1.24,
          note="195-220 C on standard printers, 200-230 C on fast printers (Bambu Lab), bed 60 C")),
    ("STLFLIX PLA Silk", "STLFLIX", "Generic PLA Silk",
     dict(nozzle=225, first=225, low=200, high=240, bed=60, density=1.24,
          note="200-230 C on fast printers (blue: 210-240 C), bed 60 C")),
    ("STLFLIX PETG", "STLFLIX", "Generic PETG",
     dict(note="the manufacturer gives no temperatures for its PETG: Generic PETG values")),
    ("Voolt3D PLA High Speed", "Voolt3D", "Generic PLA High Speed",
     dict(nozzle=220, first=220, low=190, high=230, bed=60, density=1.24, mvs=25,
          note="PLA High Speed Premium: 190-230 C, bed 50-70 C, 1.24 g/cm3, up to 25 mm3/s")),
    ("Voolt3D PLA Velvet", "Voolt3D", "Generic PLA",
     dict(nozzle=215, first=220, low=205, high=220, bed=70, density=1.24, mvs=19,
          note="PLA Velvet (matte): 205-220 C, bed 70-75 C, 1.24 g/cm3, 19 mm3/s")),
    ("Voolt3D PLA V-Silk", "Voolt3D", "Generic PLA Silk",
     dict(nozzle=225, first=225, low=205, high=240, bed=60, density=1.24,
          note="PLA V-Silk: 205-240 C, bed 50-70 C, 1.24 g/cm3")),
    ("Voolt3D PETG HF", "Voolt3D", "Generic PETG HF",
     dict(nozzle=240, first=240, low=220, high=250, bed=70, density=1.27, mvs=20,
          note="PETG High Fluidity: 220-250 C, bed 70 C, 1.27 g/cm3, 20 mm3/s")),
    ("GTMax3D PLA Speed+", "GTMax3D", "Generic PLA High Speed",
     dict(nozzle=215, first=220, low=190, high=220, bed=60,
          note="PLA Speed+: 190-220 C, bed 0-60 C, up to 500 mm/s")),
    ("GTMax3D PETG HF", "GTMax3D", "Generic PETG HF",
     dict(nozzle=245, first=245, low=225, high=270, bed=80,
          note="PETG HF: 225-270 C, bed 80-95 C")),
    ("GTMax3D ABS Speed+", "GTMax3D", "Generic ABS",
     dict(note="the manufacturer gives no temperatures for ABS Speed+: Generic ABS values")),
    ("National3D PLA Max High Speed", "National3D", "Generic PLA High Speed",
     dict(nozzle=210, first=210, low=185, high=210, bed=60, density=1.24, mvs=29,
          note="PLA MAX High Speed: 185-210 C, bed 50-70 C, 1.24 g/cm3, up to 29 mm3/s")),
    ("National3D ABS MG-94", "National3D", "Generic ABS",
     dict(nozzle=250, first=255, low=220, high=260, bed=100, density=1.04,
          note="ABS Premium MG-94: 220-260 C, bed 90-120 C, 1.04 g/cm3")),
    ("Masterprint PLA", "Masterprint", "Generic PLA",
     dict(nozzle=215, first=220, low=190, high=230, bed=55, density=1.24, mvs=25,
          note="190-230 C, bed 40-60 C, 1.24 g/cm3, up to 25 mm3/s")),
]

BED_KEYS = ["hot_plate_temp", "hot_plate_temp_initial_layer", "textured_plate_temp", "textured_plate_temp_initial_layer"]


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def newline_of(path):
    with open(path, "rb") as fh:
        return "\r\n" if b"\r\n" in fh.read(4096) else "\n"


def save(path, data, newline="\n"):
    # the bundle files keep the line ends they have in the repository
    with open(path, "w", encoding="utf-8", newline=newline) as fh:
        json.dump(data, fh, indent=4, ensure_ascii=False)
        fh.write("\n")


def slots(profile):
    """How many values a list setting has in this profile (2 on dual-extruder printers)."""
    n = 1
    for key, value in profile.items():
        if key in ("compatible_printers", "include") or not isinstance(value, list):
            continue
        n = max(n, len(value))
    return n


def brand_values(spec, base=None):
    """setting -> value (one value; spread to the profile's slots later)."""
    v = {}
    if spec.get("nozzle"):
        v["nozzle_temperature"] = spec["nozzle"]
        v["nozzle_temperature_initial_layer"] = spec.get("first", spec["nozzle"])
        v["nozzle_temperature_range_low"] = spec["low"]
        v["nozzle_temperature_range_high"] = spec["high"]
    if spec.get("bed"):
        for k in BED_KEYS:
            v[k] = spec["bed"]
    if spec.get("density"):
        v["filament_density"] = spec["density"]
    return v


def fmt(x):
    return ("%g" % x) if isinstance(x, float) else str(x)


def apply(profile, values, mvs, only_existing):
    n = slots(profile)
    for key, value in values.items():
        if only_existing and key not in profile:
            continue
        cur = profile.get(key)
        count = len(cur) if isinstance(cur, list) else n
        profile[key] = [fmt(value)] * count
    if mvs and "filament_max_volumetric_speed" in profile:
        # never faster than MiniMe's own tuning for this printer and nozzle
        profile["filament_max_volumetric_speed"] = [fmt(min(float(x), mvs)) for x in profile["filament_max_volumetric_speed"]]


def family_files(family):
    out = []
    for f in sorted(os.listdir(FILAMENT_DIR)):
        if not f.endswith(".json"):
            continue
        stem = f[:-5]
        if stem == family or stem.startswith(family + " @"):
            if stem != family + " @base":
                out.append(stem)
    return out


def main():
    bundle = load(BBL_JSON)
    bundle_newline = newline_of(BBL_JSON)
    file_newline = newline_of(os.path.join(FILAMENT_DIR, "Generic PLA @base.json"))
    made_names = []
    setting_no = 0
    # remove what an earlier run made
    ours = tuple(name for name, _, _, _ in LINES)
    bundle["filament_list"] = [e for e in bundle["filament_list"] if not (e["name"].startswith(ours))]
    for f in os.listdir(FILAMENT_DIR):
        if f.startswith(ours):
            os.remove(os.path.join(FILAMENT_DIR, f))

    for name, vendor, family, spec in LINES:
        generic_base = load(os.path.join(FILAMENT_DIR, family + " @base.json"))
        values = brand_values(spec)
        mvs = spec.get("mvs")
        # the line's base: the Generic base plus the manufacturer's data
        base = {
            "type": "filament",
            "name": name + " @base",
            "inherits": family + " @base",
            "from": "system",
            "instantiation": "false",
            "description": name + " (" + spec["note"] + "). Built on MiniMe's " + family + " profile; tune flow and pressure "
                           "advance for your printer.",
            "filament_vendor": [vendor],
        }
        apply(base, values, None, only_existing=False)
        if mvs and "filament_max_volumetric_speed" in generic_base:
            base["filament_max_volumetric_speed"] = [fmt(min(float(x), mvs)) for x in generic_base["filament_max_volumetric_speed"]]
        save(os.path.join(FILAMENT_DIR, name + " @base.json"), base, file_newline)
        bundle["filament_list"].append({"name": name + " @base", "sub_path": "filament/" + name + " @base.json"})
        made_names.append(name + " @base")

        # one profile per printer, as the Generic family has them
        for stem in family_files(family):
            src = load(os.path.join(FILAMENT_DIR, stem + ".json"))
            prof = dict(src)
            new_name = name + stem[len(family):]
            prof["name"] = new_name
            inh = src.get("inherits", "")
            prof["inherits"] = name + inh[len(family):] if inh.startswith(family) else name + " @base"
            setting_no += 1
            prof["setting_id"] = "MMBR%03d" % setting_no
            prof.pop("filament_id", None)
            prof.pop("filament_vendor", None)
            apply(prof, values, mvs, only_existing=True)
            # values measured on a real printer win over the manufacturer's data
            cal = spec.get("calibrated", {}).get(stem[len(family):].strip())
            if cal:
                cal_values = brand_values(dict(cal, low=spec["low"], high=spec["high"]))
                if cal.get("flow"):
                    cal_values["filament_flow_ratio"] = cal["flow"]
                apply(prof, cal_values, None, only_existing=False)
                prof["filament_notes"] = "Calibrated on a Bambu Lab A1 (0.4 nozzle): flow ratio %g at %d C." % (cal["flow"], cal["nozzle"])
            save(os.path.join(FILAMENT_DIR, new_name + ".json"), prof, file_newline)
            made_names.append(new_name)
        # children must come after what they inherit from
        children = [n for n in made_names if n.startswith(name) and n != name + " @base"]
        def depth(n):
            d, cur = 0, load(os.path.join(FILAMENT_DIR, n + ".json"))
            while cur.get("inherits", "").startswith(name) and not cur["inherits"].endswith("@base"):
                d += 1
                cur = load(os.path.join(FILAMENT_DIR, cur["inherits"] + ".json"))
            return d
        for n in sorted(children, key=depth):
            bundle["filament_list"].append({"name": n, "sub_path": "filament/" + n + ".json"})

    bundle["version"] = BUNDLE_VERSION
    save(BBL_JSON, bundle, bundle_newline)
    print("made %d profiles for %d filament lines; BBL.json version %s" % (len(made_names), len(LINES), BUNDLE_VERSION))


if __name__ == "__main__":
    main()
