# Part of MiniMe Slicer: see tools/orca_profiles/README.md
# Converts OrcaSlicer's third-party printer profiles into MiniMe (Bambu Studio) profiles.
# usage: python convert_orca.py <repo> <orca profiles dir> <out dir> <report file>
import hashlib, json, os, re, shutil, sys
from collections import Counter, defaultdict, OrderedDict

REPO, ORCA, OUT, REPORT = sys.argv[1:5]
SKIP_VENDORS = {'BBL', 'OrcaFilamentLibrary', 'OrcaArena'}
SKIP_CUSTOM_MODELS = {'MyBeltPrinter', 'MyToolChanger'}   # belt and tool changer machines need Orca-only features
LIB = 'OrcaFilamentLibrary'

# ---------------------------------------------------------------- MiniMe's options
src = open(os.path.join(REPO, 'src/libslic3r/PrintConfig.cpp'), encoding='utf-8', errors='replace').read()
enum_maps = {}
for m in re.finditer(r't_config_enum_values\s+s_keys_map_(\w+)\s*(?:=\s*)?\{(.*?)\n\};', src, re.S):
    enum_maps[m.group(1)] = set(re.findall(r'\{\s*"([^"]+)"\s*,', m.group(2)))
opts = {}          # key -> (type, enum name or None)
parts = re.split(r'(?=def\s*=\s*this->add\()', src)
for p in parts:
    m = re.match(r'def\s*=\s*this->add\(\s*"([^"]+)"\s*(\+\s*axis\.name)?\s*,\s*(co\w+)\s*\)', p)
    if not m:
        continue
    key, axis, typ = m.group(1), m.group(2), m.group(3)
    body = p[:4000]
    em = re.search(r'ConfigOptionEnum<(\w+)>::get_enum_values', body)
    enum = em.group(1) if em else None
    if axis:
        for a in ('x', 'y', 'z', 'e'):
            opts[key + a.upper()] = (typ, enum)
            opts[key + a] = (typ, enum)
    else:
        opts[key] = (typ, enum)

# keys Bambu Lab's own profiles use that are not print options (name, inherits, ...)
meta = set()
for root, _, files in os.walk(os.path.join(REPO, 'resources/profiles/BBL')):
    for f in files:
        if f.endswith('.json'):
            try:
                j = json.load(open(os.path.join(root, f), encoding='utf-8-sig'))
            except Exception:
                continue
            if isinstance(j, dict) and 'type' in j:
                meta |= {k for k in j if k not in opts}
meta |= {'type', 'name', 'inherits', 'from', 'setting_id', 'instantiation', 'filament_id', 'description', 'renamed_from',
         'compatible_printers', 'compatible_printers_condition', 'compatible_prints', 'compatible_prints_condition', 'alias'}
# machine_model keys
model_keys = {'type', 'name', 'machine_tech', 'family', 'model_id', 'nozzle_diameter', 'bed_model', 'bed_texture', 'hotend_model',
              'default_materials', 'default_bed_type', 'image_bed_type', 'url', 'bed_texture_size'}

RENAMES = {'extruder_clearance_radius': 'extruder_clearance_max_radius'}
# per-feature filaments: Orca's process default is 1 (filament 1); MiniMe allows them only on objects and resets them to 0 in a
# process profile, which would leave the profile "modified" as soon as it is selected
PROCESS_DROP = {'wall_filament', 'sparse_infill_filament', 'solid_infill_filament'}
# Orca's names for values MiniMe has under another name
_pattern = {'rectilinear': 'zig-zag'}
VALUE_MAP = {
    'ensure_vertical_shell_thickness': {'ensure_all': 'enabled', 'ensure_moderate': 'partial', 'ensure_critical_only': 'partial',
                                        'none': 'disabled', '1': 'enabled', '0': 'disabled'},
    'sparse_infill_pattern': _pattern, 'internal_solid_infill_pattern': _pattern, 'top_surface_pattern': _pattern,
    'bottom_surface_pattern': _pattern, 'ironing_pattern': _pattern, 'support_ironing_pattern': _pattern,
    'support_style': {'organic': 'tree_organic'},
    'seam_position': {'aligned_back': 'back'},
    'host_type': {'moonraker': 'octoprint'},   # Moonraker answers OctoPrint's upload API
}

report = defaultdict(Counter)
SCALAR = {'coFloat', 'coInt', 'coBool', 'coString', 'coPercent', 'coFloatOrPercent', 'coEnum', 'coPoint', 'coPoint3'}

def check_value(key, val, where):
    typ, enum = opts[key]
    if typ == 'coPoint' and isinstance(val, list) and len(val) == 2:
        val = f'{val[0]}x{val[1]}'     # a point written as [x, y]
    if typ in SCALAR and isinstance(val, list):
        if len(val) == 1:
            val = val[0]
        else:
            report['scalar given a list (dropped)'][key] += 1
            return None
    if enum:
        vm = VALUE_MAP.get(key, {})
        if isinstance(val, list):
            val = [vm.get(str(v), v) for v in val]
        else:
            val = vm.get(str(val), val)
        vals = val if isinstance(val, list) else ([val] if typ == 'coEnum' else str(val).split(','))
        known = enum_maps.get(enum)
        if known is None:
            report['enum map not found'][enum] += 1
            return val
        bad = [v for v in vals if str(v) not in known]
        if bad:
            report['unknown enum value (dropped)'][f'{key}={bad[0]}'] += 1
            return None
    return val

def _orient(a, b, c):
    v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return (v > 1e-12) - (v < -1e-12)

def _valid_outline(P):
    # OrcaSlicer's is_valid_bed_exclude_area: one simple outline, no repeated points, not crossing itself
    if len(P) < 3 or len(set(P)) != len(P):
        return False
    n = len(P)
    if abs(sum(P[i][0] * P[(i + 1) % n][1] - P[(i + 1) % n][0] * P[i][1] for i in range(n))) < 1e-9:
        return False
    for i in range(n - 2):
        for k in range(i + 2, n - 1 if i == 0 else n):
            a, b, c, d = P[i], P[i + 1], P[k], P[(k + 1) % n]
            if _orient(c, d, a) * _orient(c, d, b) < 0 and _orient(a, b, c) * _orient(a, b, d) < 0:
                return False
    return True

def convert_exclude_area(v):
    # Orca reads bed_exclude_area as one outline and ignores an invalid one; MiniMe reads it as rectangles of 4 points.
    # A rectangle is kept; anything Orca would ignore becomes "none" (MiniMe would otherwise read wrong rectangles from it).
    items = v if isinstance(v, list) else [v]
    items = [x.strip() for x in ','.join(str(x) for x in items).split(',') if x.strip()]
    try:
        P = [tuple(float(c) for c in x.split('x')) for x in items]
    except ValueError:
        report['bed_exclude_area cleared'][str(v)[:60]] += 1
        return ['0x0']
    if len(P) <= 1:
        return ['0x0']
    if _valid_outline(P) and len(P) == 4 and len({p[0] for p in P}) == 2 and len({p[1] for p in P}) == 2:
        return items
    report['bed_exclude_area cleared'][f'{len(P)} points'] += 1
    return ['0x0']

def convert_preset(j, where):
    out = OrderedDict()
    # (settings placed in the wrong kind of profile are left in: MiniMe removes them itself when it loads the profile)
    for k, v in j.items():
        k2 = RENAMES.get(k, k)
        if j.get('type') == 'process' and k2 in PROCESS_DROP:
            report['process setting MiniMe only allows per object (dropped)'][k2] += 1
            continue
        if k2 == 'bed_exclude_area':
            out[k2] = convert_exclude_area(v)
            continue
        if k2 in opts:
            v2 = check_value(k2, v, where)
            if v2 is not None:
                out[k2] = v2
        elif k2 in meta:
            out[k2] = v
        else:
            report['unknown key (dropped)'][k] += 1
    return out

PCT_OF_OUTER_WALL_SPEED = {'overhang_1_4_speed', 'overhang_2_4_speed', 'overhang_3_4_speed', 'overhang_4_4_speed'}

def _is_pct(v):
    vals = v if isinstance(v, list) else [v]
    return any(isinstance(x, str) and x.strip().endswith('%') for x in vals)

def _first_number(v):
    x = v[0] if isinstance(v, list) and v else v
    try:
        return float(str(x).strip().rstrip('%'))
    except ValueError:
        return None

def resolve_percentages(converted, vendor):
    # Orca writes some settings as a percentage of another one (line widths of the nozzle, overhang speeds of the outer wall
    # speed); MiniMe takes those settings in mm or mm/s only and would read "110%" as 110. Work them out per printer the way
    # Orca does, and drop the percentages MiniMe cannot read (the parent profile's or MiniMe's default applies instead).
    machines = {j['name']: j for _, j in converted.get('machine_list', [])}
    procs = {j['name']: j for _, j in converted.get('process_list', [])}

    def machine_nozzle(name, depth=0):
        j = machines.get(name)
        if j is None or depth > 20:
            return None
        if 'nozzle_diameter' in j:
            return _first_number(j['nozzle_diameter'] if isinstance(j['nozzle_diameter'], list) else str(j['nozzle_diameter']).split(','))
        return machine_nozzle(j.get('inherits'), depth + 1)

    def resolved(j, key):
        depth = 0
        while j is not None and depth < 20:
            if key in j:
                return j[key]
            j = procs.get(j.get('inherits'))
            depth += 1
        return None

    def chain_keys(j):
        keys, depth = set(), 0
        while j is not None and depth < 20:
            keys |= set(j)
            j = procs.get(j.get('inherits'))
            depth += 1
        return keys

    for _, j in converted.get('process_list', []):
        if str(j.get('instantiation', 'true')).lower() != 'true':
            continue
        nozzle = None
        cps = j.get('compatible_printers') or []
        for cp in (cps if isinstance(cps, list) else [cps]):
            nozzle = nozzle or machine_nozzle(cp)
        if nozzle is None:
            m = re.search(r'(\d+(?:\.\d+)?)\s*(?:mm\s*)?nozzle', j['name'], re.I)
            nozzle = float(m.group(1)) if m else 0.4
        for key in chain_keys(j):
            if key not in opts or opts[key][0] not in ('coFloat', 'coFloats'):
                continue
            v = resolved(j, key)
            if not _is_pct(v):
                continue
            pct, val = _first_number(v), None
            if pct is None:
                continue
            if key.endswith('line_width'):
                val = round(pct / 100. * nozzle, 3)
            elif key in PCT_OF_OUTER_WALL_SPEED:
                base = resolved(j, 'outer_wall_speed')
                if base is not None and not _is_pct(base) and _first_number(base) is not None:
                    val = round(pct / 100. * _first_number(base), 1)
            if val is not None:
                j[key] = [f'{val:g}'] if opts[key][0] == 'coFloats' else f'{val:g}'
                report['percentage worked out'][key] += 1

    for lst, items in converted.items():
        if lst == 'machine_model_list':
            continue
        for _, j in items:
            for k in list(j.keys()):
                if k in opts and _is_pct(j[k]):
                    typ = opts[k][0]
                    if typ in ('coFloat', 'coFloats'):
                        del j[k]
                        report['percentage MiniMe cannot read (dropped)'][k] += 1
                    elif typ in ('coInt', 'coInts'):
                        # fan speeds and the like: already a percentage, written with a % sign
                        j[k] = [str(x).strip().rstrip('%') for x in j[k]] if isinstance(j[k], list) else str(j[k]).strip().rstrip('%')

def _points(v):
    items = v if isinstance(v, list) else [v]
    items = [x.strip() for x in ','.join(str(x) for x in items).split(',') if x.strip()]
    return [tuple(float(c) for c in x.split('x')) for x in items]

def _is_rect(P):
    return len(P) == 4 and len({round(p[0], 4) for p in P}) == 2 and len({round(p[1], 4) for p in P}) == 2

def _fmt(x):
    return f'{round(x, 2):g}'

def fix_round_beds(converted, vendor):
    # MiniMe's plates are rectangles whose first point is the front left corner. A round (delta) or other non-rectangular bed
    # becomes its bounding rectangle, and the parts of that rectangle outside the real bed become exclusion rectangles
    # (steps that only cover what is certainly outside), so objects cannot be placed where the printer cannot reach.
    machines = {j['name']: j for _, j in converted.get('machine_list', [])}

    def resolved(j, key):
        depth = 0
        while j is not None and depth < 20:
            if key in j:
                return j[key]
            j = machines.get(j.get('inherits'))
            depth += 1
        return None

    for _, j in converted.get('machine_list', []):
        area = resolved(j, 'printable_area')
        if area is None:
            continue
        try:
            P = _points(area)
        except ValueError:
            continue
        if len(P) < 3:
            continue
        xs, ys = [p[0] for p in P], [p[1] for p in P]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        if _is_rect(P):
            if P[0] != (x0, y0):   # MiniMe needs the front left corner first
                j['printable_area'] = [f'{_fmt(x0)}x{_fmt(y0)}', f'{_fmt(x1)}x{_fmt(y0)}', f'{_fmt(x1)}x{_fmt(y1)}', f'{_fmt(x0)}x{_fmt(y1)}']
                report['bed corners reordered'][vendor] += 1
            continue
        if str(j.get('instantiation', 'true')).lower() != 'true':
            continue
        n = len(P)

        def x_range_at(y):
            hits = []
            for i in range(n):
                (ax, ay), (bx, by) = P[i], P[(i + 1) % n]
                if (ay <= y <= by or by <= y <= ay) and ay != by:
                    hits.append(ax + (y - ay) * (bx - ax) / (by - ay))
            return (min(hits), max(hits)) if hits else None

        bands = 8
        rects = []
        for b in range(bands):
            ya, yb = y0 + (y1 - y0) * b / bands, y0 + (y1 - y0) * (b + 1) / bands
            lefts, rights = [], []
            for s in range(21):
                r = x_range_at(ya + (yb - ya) * s / 20)
                if r:
                    lefts.append(r[0])
                    rights.append(r[1])
            if not lefts:
                continue
            left, right = min(lefts), max(rights)     # reachable somewhere in this band
            if left - x0 > 0.5:
                rects.append((x0, ya, left, yb))
            if x1 - right > 0.5:
                rects.append((right, ya, x1, yb))
        exclude = []
        for (ax, ay, bx, by) in rects:
            exclude += [f'{_fmt(ax)}x{_fmt(ay)}', f'{_fmt(bx)}x{_fmt(ay)}', f'{_fmt(bx)}x{_fmt(by)}', f'{_fmt(ax)}x{_fmt(by)}']
        j['printable_area'] = [f'{_fmt(x0)}x{_fmt(y0)}', f'{_fmt(x1)}x{_fmt(y0)}', f'{_fmt(x1)}x{_fmt(y1)}', f'{_fmt(x0)}x{_fmt(y1)}']
        j['bed_exclude_area'] = exclude or ['0x0']
        report['round bed as rectangle + excluded corners'][vendor] += 1

# printer models that did not slice in MiniMe (tools/orca_profiles/excluded_printers.txt: "brand | model" per line)
EXCLUDED = set()
_excl = os.path.join(REPO, 'tools', 'orca_profiles', 'excluded_printers.txt')
if os.path.exists(_excl):
    for line in open(_excl, encoding='utf-8'):
        if '|' in line and not line.lstrip().startswith('#'):
            b, m = line.split('|', 1)
            EXCLUDED.add((b.strip(), m.strip()))

def drop_multi_nozzle(converted, vendor):
    # Printers with several nozzles (IDEX, tool changers) are left out: MiniMe's multi-nozzle code is made for Bambu Lab's own
    # dual-nozzle printers and crashes slicing with these profiles. Returns how many printer models were removed.
    machines = {j['name']: j for _, j in converted.get('machine_list', [])}

    def nozzles(j):
        depth = 0
        while j is not None and depth < 20:
            if 'nozzle_diameter' in j:
                v = j['nozzle_diameter']
                items = v if isinstance(v, list) else [v]
                return len([x for x in ','.join(str(x) for x in items).split(',') if x.strip()])
            j = machines.get(j.get('inherits'))
            depth += 1
        return 1

    def belt(j):
        # belt printers need OrcaSlicer's belt slicing
        depth = 0
        while j is not None and depth < 20:
            if 'belt_slice_rotation_angle' in json.dumps(j):
                return True
            j = machines.get(j.get('inherits'))
            depth += 1
        return False

    def excluded(j):
        depth, model = 0, None
        while j is not None and depth < 20 and model is None:
            model = j.get('printer_model')
            j = machines.get(j.get('inherits'))
            depth += 1
        return (vendor, model) in EXCLUDED

    dropped = {j['name'] for _, j in converted.get('machine_list', [])
               if str(j.get('instantiation', 'true')).lower() == 'true' and (nozzles(j) > 1 or belt(j) or excluded(j))}
    if not dropped:
        return 0
    # profiles built on a dropped one go too (a missing parent stops MiniMe loading the whole brand)
    grew = True
    while grew:
        grew = False
        for _, j in converted.get('machine_list', []):
            if j['name'] not in dropped and j.get('inherits') in dropped:
                dropped.add(j['name'])
                grew = True
    converted['machine_list'] = [(s, j) for s, j in converted['machine_list'] if j['name'] not in dropped]
    kept_models = {j.get('printer_model') for _, j in converted['machine_list'] if str(j.get('instantiation', 'true')).lower() == 'true'}
    before = len(converted.get('machine_model_list', []))
    converted['machine_model_list'] = [(s, j) for s, j in converted.get('machine_model_list', []) if j['name'] in kept_models]
    removed_models = before - len(converted['machine_model_list'])
    # process and filament profiles made only for the removed printers go too (unless another profile builds on them)
    for lst in ('process_list', 'filament_list'):
        items = converted.get(lst, [])
        parents = {j.get('inherits') for _, j in items}
        out = []
        for s, j in items:
            cps = j.get('compatible_printers')
            if isinstance(cps, list) and cps:
                left = [c for c in cps if c not in dropped]
                if not left and j['name'] not in parents:
                    continue
                j['compatible_printers'] = left
            out.append((s, j))
        converted[lst] = out
    report['printers with several nozzles (left out)'][vendor] += len(dropped)
    return removed_models

def load(path):
    return json.load(open(path, encoding='utf-8-sig'), object_pairs_hook=OrderedDict)

def dump(obj, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(obj, f, indent=4, ensure_ascii=False)
        f.write('\n')

# ---------------------------------------------------------------- the filament library (for vendors that rely on it)
lib_index = load(os.path.join(ORCA, LIB + '.json'))
lib = {}
for it in lib_index.get('filament_list', []):
    p = os.path.join(ORCA, LIB, it['sub_path'])
    if os.path.exists(p):
        j = load(p)
        lib[j['name']] = j

def lib_flatten(name):
    chain = []
    n = name
    while n:
        j = lib[n]
        chain.append(j)
        n = j.get('inherits')
    merged = OrderedDict()
    for j in reversed(chain):
        for k, v in j.items():
            merged[k] = v
    return merged

# ---------------------------------------------------------------- vendors
if os.path.exists(OUT):
    shutil.rmtree(OUT)
os.makedirs(OUT)
summary = []
for vf in sorted(os.listdir(ORCA)):
    if not vf.endswith('.json'):
        continue
    vendor = vf[:-5]
    if vendor in SKIP_VENDORS or not os.path.isdir(os.path.join(ORCA, vendor)):
        continue
    vj = load(os.path.join(ORCA, vf))
    vdir = os.path.join(ORCA, vendor)
    odir = os.path.join(OUT, vendor)
    defined = {}
    lists = {}
    for lst in ('machine_model_list', 'process_list', 'filament_list', 'machine_list'):
        items = []
        for it in vj.get(lst, []):
            p = os.path.join(vdir, it['sub_path'])
            if not os.path.exists(p):
                report['missing file in index'][f'{vendor}/{it["sub_path"]}'] += 1
                continue
            j = load(p)
            if vendor == 'Custom' and lst in ('machine_model_list', 'machine_list') and any(j.get('name', '').startswith(s) or j.get('printer_model', '').startswith(s) for s in SKIP_CUSTOM_MODELS):
                continue
            items.append((it, j))
            defined[j['name']] = lst
        lists[lst] = items
    # library presets this vendor refers to
    def names_in(v):
        if not v:
            return []
        if isinstance(v, str):
            v = v.split(';')
        return [x.strip() for x in v if x.strip()]
    refs = set()
    for lst, items in lists.items():
        for it, j in items:
            if j.get('inherits') and j['inherits'].strip() not in defined:
                refs.add(j['inherits'].strip())
            for n in names_in(j.get('default_filament_profile')):
                if n not in defined:
                    refs.add(n)
            if lst == 'machine_model_list':
                for n in names_in(j.get('default_materials')):
                    if n not in defined:
                        refs.add(n)
    rename = {}
    added = []
    inherit_names = {j['inherits'].strip() for items in lists.values() for _, j in items
                     if j.get('inherits') and j['inherits'].strip() not in defined}
    for n in sorted(refs):
        if n in lib and lib[n].get('instantiation') == 'true':
            if n in inherit_names:
                # profiles of this brand build on it: a hidden base copy under its own name (base profiles are per brand)
                base = lib_flatten(n)
                base.pop('inherits', None)
                base['from'] = 'system'
                base['instantiation'] = 'false'
                base.pop('compatible_printers', None)
                added.insert(0, base)
            # default filament references: the brand's own copy, shown in the filament list
            new = n.replace(' @System', '') + ' @' + vendor if ' @System' in n else n + ' @' + vendor
            rename[n] = new
            if new in defined:
                continue
            flat = lib_flatten(n)
            flat['name'] = new
            flat.pop('inherits', None)
            flat['from'] = 'system'
            flat['instantiation'] = 'true'
            flat.pop('compatible_printers', None)
            flat['setting_id'] = 'MM' + hashlib.md5((vendor + n).encode()).hexdigest()[:14]
            added.append(flat)
        elif n in lib:
            # a base preset (not shown in lists): copied under its own name, these are kept per vendor
            flat = lib_flatten(n)
            flat.pop('inherits', None)
            flat['from'] = 'system'
            flat['instantiation'] = 'false'
            added.insert(0, flat)
        else:
            report['unresolved reference'][f'{vendor}: {n}'] += 1
    def ren(v):
        if isinstance(v, list):
            return [rename.get(x.strip(), x.strip()) for x in v]
        if isinstance(v, str) and ';' in v:
            return [rename.get(x.strip(), x.strip()) for x in v.split(';') if x.strip()]
        return rename.get(v.strip(), v.strip()) if isinstance(v, str) else v
    new_index = OrderedDict((k, v) for k, v in vj.items() if not k.endswith('_list'))
    count_models = 0
    converted = OrderedDict()   # list -> [(sub_path, preset)]
    for lst in ('machine_model_list', 'process_list', 'filament_list', 'machine_list'):
        items = []
        if lst == 'filament_list':
            for flat in added:
                items.append((f'filament/{flat["name"]}.json', convert_preset(flat, vendor)))
        for it, j in lists.get(lst, []):
            if lst == 'machine_model_list':
                j2 = OrderedDict((k, v) for k, v in j.items() if k in model_keys)
                for k in set(j) - model_keys:
                    report['unknown machine_model key (dropped)'][k] += 1
                if 'default_materials' in j2:
                    j2['default_materials'] = ';'.join(rename.get(x, x) for x in j2['default_materials'].split(';'))
                count_models += 1
            else:
                j2 = convert_preset(j, vendor)
                if 'inherits' in j2:
                    j2['inherits'] = j2['inherits'].strip()   # parents keep their names (see the base copies above)
                if 'default_filament_profile' in j2:
                    j2['default_filament_profile'] = ren(j2['default_filament_profile'])
            items.append((it['sub_path'], j2))
        converted[lst] = items
    resolve_percentages(converted, vendor)
    fix_round_beds(converted, vendor)
    count_models -= drop_multi_nozzle(converted, vendor)
    if count_models <= 0:
        report['brand left out (only printers with several nozzles)'][vendor] += 1
        continue
    for lst, items in converted.items():
        out_items = []
        for sub, j2 in items:
            dump(j2, os.path.join(odir, sub))
            out_items.append(OrderedDict(name=j2['name'], sub_path=sub))
        new_index[lst] = out_items
    dump(new_index, os.path.join(OUT, vendor + '.json'))
    # pictures, bed models and textures
    for f in os.listdir(vdir):
        p = os.path.join(vdir, f)
        if os.path.isfile(p) and not f.endswith('.json'):
            if vendor == 'Custom' and any(f.startswith(s) for s in ('Generic Belt', 'Generic ToolChanger')):
                continue
            shutil.copy2(p, os.path.join(odir, f))
    summary.append(f'{vendor}: {count_models} printers, {len(lists.get("filament_list", []))} filaments (+{len(added)} from the library)')

with open(REPORT, 'w', encoding='utf-8') as f:
    f.write(f'options known: {len(opts)}, enum maps: {len(enum_maps)}\n\n')
    f.write('\n'.join(summary) + '\n')
    for cat, c in report.items():
        f.write(f'\n== {cat} ({sum(c.values())})\n')
        for k, n in c.most_common():
            f.write(f'  {n:6} {k}\n')
print('\n'.join(summary[-3:]))
print({cat: sum(c.values()) for cat, c in report.items()})
