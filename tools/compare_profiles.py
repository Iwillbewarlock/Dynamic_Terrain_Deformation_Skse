import argparse
import re
import statistics
from pathlib import Path


def read(path, warmup):
    rows, row, variant, first, reset = [], None, 'unknown', None, None
    for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        timestamp = re.match(r'\[(\d+):(\d+):(\d+\.\d+)\]', line)
        if not timestamp:
            continue
        h, m, s = map(float, timestamp.groups())
        now = h * 3600 + m * 60 + s
        if 'Settings changed on disk' in line:
            reset = now
        if 'Optimization test variant:' in line:
            variant = line.split('Optimization test variant:', 1)[1].strip()
        header = re.search(r'--- Profile: (\d+) frames', line)
        if header:
            if row:
                rows.append(row)
            if first is None:
                first = now
            row = {'time': now, 'frames': int(header[1]), 'variant': variant,
                   'warm': now - max(first, reset if reset is not None else first) >= warmup,
                   'tess': re.search(r'tess=(\w+)', line)[1], 'values': {}, 'builds': False}
        if row is None:
            continue
        metric = re.search(r'\[I\]\s+(.+?)\s+avg\s+([\d.]+) ms', line)
        if metric:
            row['values'][metric[1].strip()] = float(metric[2])
        geometry = re.search(r'Geometry\s+HS\s+([\d.]+)\s+DS\s+([\d.]+)', line)
        if geometry:
            row['values']['DS invocations/frame'] = float(geometry[2])
        if 'Shader builds' in line:
            row['builds'] = True
    if row:
        rows.append(row)
    return [r for r in rows if r['warm'] and not r['builds'] and r['tess'] == 'true']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--warmup', type=float, default=30,
                        help='seconds discarded after first report or a settings reload')
    args = parser.parse_args()
    for path in args.logs:
        rows = read(path, args.warmup)
        print(f'\n{path.name}: {len(rows)} eligible report windows')
        if not rows:
            continue
        print('Variants:', ', '.join(sorted({r['variant'] for r in rows})))
        print('Metric | frame-weighted mean | median window | window range')
        for name in sorted({k for r in rows for k in r['values']}):
            samples = [(r['values'][name], r['frames']) for r in rows if name in r['values']]
            values = [v for v, _ in samples]
            mean = sum(v * n for v, n in samples) / sum(n for _, n in samples)
            print(f'{name}: {mean:.3f} | {statistics.median(values):.3f} | {min(values):.3f}–{max(values):.3f}')
    print('\nTimings are ms; DS is a count. Compare identical scenes/settings only.')
    print('Window ranges are not per-frame percentiles. A/B/A repetition is needed for small differences.')


if __name__ == '__main__':
    main()
