# Part of MiniMe Slicer: see tools/orca_profiles/README.md
# Slices a 20 mm cube on one profile of every printer model through MiniMe's Claude bridge.
# usage: python sweep.py <profiles dir> <cube.stl> <results.jsonl> [vendor filter]
import json, os, sys, time, urllib.request

PROFILES, CUBE, OUT = sys.argv[1:4]
ONLY = sys.argv[4].split(',') if len(sys.argv) > 4 else None
URL = 'http://127.0.0.1:13700/mcp'

def call(tool, args, timeout=300):
    body = json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call', 'params': {'name': tool, 'arguments': args}}).encode()
    req = urllib.request.Request(URL, data=body, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        j = json.loads(r.read().decode('utf-8'))
    if 'error' in j:
        return False, j['error'].get('message')
    res = j['result']
    text = ''.join(c.get('text', '') for c in res.get('content', []) if c.get('type') == 'text')
    try:
        data = json.loads(text)
    except Exception:
        data = text
    return not res.get('isError', False), data

def load(p):
    return json.load(open(p, encoding='utf-8-sig'))

# one machine preset per printer model, the 0.4 mm nozzle if there is one
printers = []
for vf in sorted(os.listdir(PROFILES)):
    if not vf.endswith('.json') or vf == 'blacklist.json':
        continue
    vendor = vf[:-5]
    if ONLY and vendor not in ONLY:
        continue
    vj = load(os.path.join(PROFILES, vf))
    by_model = {}
    for it in vj.get('machine_list', []):
        p = os.path.join(PROFILES, vendor, it['sub_path'])
        if not os.path.exists(p):
            continue
        j = load(p)
        if str(j.get('instantiation', 'true')).lower() != 'true':
            continue
        model = j.get('printer_model') or j['name']
        by_model.setdefault(model, []).append(j['name'])
    for model, names in by_model.items():
        pick = next((n for n in names if '0.4' in n), names[0])
        printers.append((vendor, model, pick))
print(len(printers), 'printers', flush=True)

done = set()
if os.path.exists(OUT):
    for line in open(OUT, encoding='utf-8'):
        try:
            done.add(json.loads(line)['preset'])
        except Exception:
            pass

ok, scene = call('get_scene', {})
if not ok or not scene.get('objects'):
    print(call('add_model', {'path': CUBE}))

with open(OUT, 'a', encoding='utf-8') as out:
    for vendor, model, name in printers:
        if name in done:
            continue
        rec = {'vendor': vendor, 'model': model, 'preset': name}
        t0 = time.time()
        try:
            ok, r = call('set_printer', {'printer': name})
            if not ok:
                rec.update(stage='set_printer', error=r)
            else:
                rec['filaments'] = r.get('filaments')
                bed = r.get('bed_mm') or [100, 100]
                center = r.get('bed_center_on_plate_mm') or [bed[0] / 2, bed[1] / 2]
                call('transform_object', {'object': 0, 'center_on_plate_mm': center})
                for attempt in range(6):
                    ok, s = call('slice', {'wait_seconds': 45})
                    if ok and isinstance(s, dict) and s.get('state') == 'still slicing':
                        continue
                    break
                if not ok:
                    rec.update(stage='slice', error=s)
                else:
                    errs = [w for w in (s.get('warnings') or []) if w.get('level') == 'error']
                    rec.update(stage='ok', print_time=s.get('print_time'), filament_g=s.get('filament_g'), errors=errs,
                               warnings=[w.get('message') for w in (s.get('warnings') or []) if w.get('level') == 'warning'])
        except Exception as e:
            rec.update(stage='exception', error=str(e))
        rec['seconds'] = round(time.time() - t0, 1)
        out.write(json.dumps(rec, ensure_ascii=False) + '\n')
        out.flush()
        print(f"{rec['stage']:12} {name}  {str(rec.get('error', ''))[:150]}", flush=True)
        if 'unsaved changes' in str(rec.get('error', '')):
            print('stopping: a profile was left modified (see the error above)', flush=True)
            break
