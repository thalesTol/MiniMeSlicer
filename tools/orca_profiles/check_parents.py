# Part of MiniMe Slicer: see tools/orca_profiles/README.md
# Every profile's parent must be listed before it in the same brand, or MiniMe fails to load the whole brand.
import glob, json, os, sys
root = sys.argv[1]
bad = 0
for vf in glob.glob(os.path.join(root, '*.json')):
    v = os.path.basename(vf)[:-5]
    try:
        idx = json.load(open(vf, encoding='utf-8'))
    except Exception:
        continue
    if not isinstance(idx, dict):
        continue
    for lst in ('machine_list', 'process_list', 'filament_list'):
        names = set()
        for it in idx.get(lst, []):
            p = os.path.join(root, v, it['sub_path'])
            if not os.path.exists(p):
                bad += 1
                print('missing file', v, it['sub_path'])
                continue
            j = json.load(open(p, encoding='utf-8'))
            parent = j.get('inherits')
            if parent and parent not in names:
                bad += 1
                print('missing parent', v, lst, j['name'], '->', parent)
            names.add(j['name'])
print('problems:', bad)
