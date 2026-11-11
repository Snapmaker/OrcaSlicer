"""Audit Snorca G-code without equating byte differences with geometry changes.

Handles modal XYZ/E and XY I/J arcs. Printer macros are retained in the command
hash but not executed. Geometry covers positive XY extrusion outside Custom
roles; it is not a firmware simulator or a proof of printability. Unsupported
geometry fails explicitly. Requires numpy; spatial comparisons also use shapely.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import re

import numpy as np

WORD = re.compile(r"([XYZEFIJR])([-+]?(?:\d*\.\d+|\d+\.?\d*))")


def read_gcode(path: Path):
    absolute, relative_e = True, False
    x = y = None
    z = e_position = feed = 0.0
    tool, role, height, width = 0, 'Custom', 0.0, 0.0
    rows = defaultdict(list)
    settings, stats, tool_sequence, layers = {}, {}, [], []
    commands = hashlib.sha256()
    in_config = False
    arc_count = 0
    with path.open(encoding='utf-8') as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.rstrip('\r\n')
            if line == '; CONFIG_BLOCK_START': in_config = True
            if in_config:
                if line.startswith('; ') and ' = ' in line:
                    k, v = line[2:].split(' = ', 1)
                    settings[k] = v
                continue
            if line.startswith(';TYPE:'): role = line[6:].strip()
            elif line.startswith(';HEIGHT:'): height = float(line[8:])
            elif line.startswith(';WIDTH:'): width = float(line[7:])
            elif line.startswith(';Z:'): layers.append(float(line[3:]))
            elif line.startswith('; ') and ' = ' in line:
                k, v = line[2:].split(' = ', 1)
                stats[k] = v
            code = line.split(';', 1)[0].strip()
            if not code: continue
            commands.update((code + '\n').encode())
            cmd = code.split()[0]
            if cmd == 'G90': absolute = True
            elif cmd == 'G91': absolute = False
            elif cmd == 'M82': relative_e = False
            elif cmd == 'M83': relative_e = True
            elif cmd in ('G20', 'G18', 'G19', 'G90.1'):
                raise ValueError(f'{path}:{line_number}: unsupported modal command {cmd}')
            elif re.fullmatch(r'T\d+', cmd):
                tool = int(cmd[1:]); tool_sequence.append(tool)
            elif cmd in ('G0', 'G1', 'G2', 'G3', 'G92'):
                v = {k: float(value) for k, value in WORD.findall(code)}
                if cmd == 'G92':
                    if any(k in v for k in 'XYZ'):
                        raise ValueError(f'{path}:{line_number}: XYZ origin reset needs explicit coordinate-offset handling')
                    e_position = v.get('E', e_position)
                    continue
                nx = v.get('X', x) if absolute else (None if x is None else x + v.get('X', 0))
                ny = v.get('Y', y) if absolute else (None if y is None else y + v.get('Y', 0))
                nz = v.get('Z', z) if absolute else z + v.get('Z', 0)
                delta_e = v.get('E', 0) if relative_e else v.get('E', e_position) - e_position
                e_position += delta_e
                feed = v.get('F', feed)
                if role != 'Custom' and delta_e > 0 and ('X' in v or 'Y' in v or cmd in ('G2', 'G3')):
                    if None in (x, y, nx, ny): raise ValueError(f'{path}:{line_number}: unknown XY origin')
                    key = (tool, round(nz, 6), role, round(height, 6), round(width, 6))
                    if cmd in ('G2', 'G3'):
                        if 'R' in v: raise ValueError(f'{path}:{line_number}: radius-form arc unsupported')
                        cx, cy = x + v.get('I', 0), y + v.get('J', 0)
                        radius = math.hypot(x - cx, y - cy)
                        if radius == 0: raise ValueError('Zero-radius arc')
                        a0, a1 = math.atan2(y-cy, x-cx), math.atan2(ny-cy, nx-cx)
                        sweep = (a1-a0) % (2*math.pi) if cmd == 'G3' else -((a0-a1) % (2*math.pi))
                        if abs(sweep) < 1e-12: sweep = 2*math.pi if cmd == 'G3' else -2*math.pi
                        # Maximum chord deviation 0.001 mm; retain total extrusion.
                        step = 2*math.acos(max(-1.0, 1-min(0.001/radius, 1.0)))
                        count = max(1, math.ceil(abs(sweep)/step))
                        points = [(x,y)] + [(cx+radius*math.cos(a0+sweep*i/count), cy+radius*math.sin(a0+sweep*i/count)) for i in range(1,count)] + [(nx,ny)]
                        for i in range(count):
                            rows[key].append((*points[i], *points[i+1], delta_e/count, feed))
                        arc_count += 1
                    elif nx != x or ny != y:
                        rows[key].append((x,y,nx,ny,delta_e,feed))
                x,y,z = nx,ny,nz
    arrays = {k: np.asarray(v, dtype=np.float64) for k,v in rows.items()}
    totals = defaultdict(float)
    role_totals = defaultdict(lambda: {'e_mm':0.0, 'length_mm':0.0, 'segments':0})
    for k, a in arrays.items():
        totals[k[0]] += float(a[:,4].sum())
        row = role_totals[k[2]]
        row['e_mm'] += float(a[:,4].sum())
        row['length_mm'] += float(np.linalg.norm(a[:,:2]-a[:,2:4],axis=1).sum())
        row['segments'] += len(a)
    summary = {'file': str(path.resolve()), 'bytes': path.stat().st_size,
               'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
               'command_sha256': commands.hexdigest(), 'settings': settings, 'statistics': stats,
               'tool_sequence': tool_sequence, 'layer_z': layers, 'arc_commands': arc_count,
               'deposited_e_mm_by_tool': dict(totals), 'group_count': len(arrays),
               'deposited_by_role': dict(role_totals),
               'segments': sum(len(a) for a in arrays.values())}
    return summary, arrays


def canonical(a):
    """Direction/order-independent segments, retaining extrusion and feed rate."""
    a = a.copy()
    reverse = (a[:,0] > a[:,2]) | ((a[:,0] == a[:,2]) & (a[:,1] > a[:,3]))
    a[reverse,:4] = a[reverse,:4][:,[2,3,0,1]]
    a = np.round(a, 6)
    return Counter(map(tuple, a))


def compare(left, right, spatial=False, tolerance=0.01):
    sa, aa = left; sb, ab = right
    result = {'same_commands': sa['command_sha256'] == sb['command_sha256'],
              'same_tool_sequence': sa['tool_sequence'] == sb['tool_sequence'],
              'same_layer_sequence': sa['layer_z'] == sb['layer_z'],
              'settings_differences': {k: [sa['settings'].get(k), sb['settings'].get(k)]
                 for k in sorted(sa['settings'].keys() | sb['settings'].keys())
                 if sa['settings'].get(k) != sb['settings'].get(k)},
              'deposited_e_delta_mm_by_tool': {k: sb['deposited_e_mm_by_tool'].get(k,0)-sa['deposited_e_mm_by_tool'].get(k,0)
                 for k in sorted(sa['deposited_e_mm_by_tool'].keys() | sb['deposited_e_mm_by_tool'].keys())},
              'groups': [], 'tolerance_mm': tolerance if spatial else None}
    if spatial:
        import shapely
    for key in sorted(aa.keys() | ab.keys()):
        a = aa.get(key, np.empty((0,6))); b = ab.get(key, np.empty((0,6)))
        ca, cb = canonical(a), canonical(b)
        if ca == cb: continue
        row = {'tool': key[0], 'z': key[1], 'role': key[2], 'height': key[3], 'width': key[4],
               'before_segments': len(a), 'after_segments': len(b),
               'unmatched_before': sum((ca-cb).values()), 'unmatched_after': sum((cb-ca).values()),
               'e_delta_mm': float(b[:,4].sum()-a[:,4].sum())}
        result['groups'].append(row)
    result['changed_groups'] = len(result['groups'])
    result['sum_absolute_group_e_delta_mm'] = sum(abs(r['e_delta_mm']) for r in result['groups'])
    if spatial:
        # Width stays in the strict comparison above. Spatial coverage merges
        # widths, so a nanometre-level width change is not reported as a wholly
        # missing path. Preserve material, physical Z, role and layer thickness.
        changed = {(r['tool'],r['z'],r['role'],r['height']) for r in result['groups']}
        buckets_a, buckets_b = defaultdict(list), defaultdict(list)
        for k,a in aa.items():
            if k[:4] in changed: buckets_a[k[:4]].append(a)
        for k,b in ab.items():
            if k[:4] in changed: buckets_b[k[:4]].append(b)
        coverage = []
        for key in sorted(changed):
            a = np.concatenate(buckets_a[key]) if buckets_a[key] else np.empty((0,6))
            b = np.concatenate(buckets_b[key]) if buckets_b[key] else np.empty((0,6))
            ga = shapely.multilinestrings(shapely.linestrings(a[:,:4].reshape(-1,2,2))) if len(a) else shapely.GeometryCollection()
            gb = shapely.multilinestrings(shapely.linestrings(b[:,:4].reshape(-1,2,2))) if len(b) else shapely.GeometryCollection()
            coverage.append({'tool':key[0], 'z':key[1], 'role':key[2], 'height':key[3],
                             'before_length_mm':ga.length, 'after_length_mm':gb.length,
                             'missing_length_mm':ga.difference(gb.buffer(tolerance,quad_segs=4)).length,
                             'added_length_mm':gb.difference(ga.buffer(tolerance,quad_segs=4)).length})
        result['coverage_groups'] = coverage
        result['missing_length_mm'] = sum(r['missing_length_mm'] for r in coverage)
        result['added_length_mm'] = sum(r['added_length_mm'] for r in coverage)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--spatial', action='store_true')
    parser.add_argument('--tolerance', type=float, default=0.01)
    args = parser.parse_args()
    a, b = read_gcode(args.before), read_gcode(args.after)
    comparison = compare(a,b,args.spatial,args.tolerance)
    args.output.write_text(json.dumps({'before':a[0], 'after':b[0], 'comparison':comparison}, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in comparison.items() if k not in ('groups','coverage_groups','settings_differences')}, indent=2))
    return 0 if comparison['same_commands'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
