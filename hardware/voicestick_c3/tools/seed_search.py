#!/usr/bin/env python3
"""Route the board with several UUID seed sets and rank the results.

The seed set changes the order KRT sees the parts and nets in, so each set is
a different, equally legal routing. Every set is built in its own copy of
hardware/voicestick_c3 (make.sh board: schematic -> PCB -> routing -> DRC -> checks)
and scored with tools/board_metrics.py.

    python3 tools/seed_search.py --sets 0-9 --jobs 5

Ranking: boards that fail any check are last; then less narrow power copper,
fewer signal vias, shorter signal copper, more outer GND pour. Freeze the winner
by setting KIID_SEED_SET's default in tools/design.py and re-running ./make.sh.
"""
import argparse
import json
import os
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORK = Path('/tmp/voicestick-seeds')
KPY = '/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3'
SKIP = ('build', 'fab', 'out', '__pycache__')


def parse_sets(text):
    sets = []
    for chunk in text.split(','):
        if '-' in chunk:
            a, b = chunk.split('-')
            sets += range(int(a), int(b) + 1)
        else:
            sets.append(int(chunk))
    return sets


def run_set(seed_set):
    copy = WORK / f'set-{seed_set}'
    shutil.rmtree(copy, ignore_errors=True)
    shutil.copytree(ROOT, copy, ignore=shutil.ignore_patterns(*SKIP))
    (copy / 'build').mkdir()
    env = dict(os.environ, KIID_SEED_SET=str(seed_set))
    start = time.time()
    with open(copy / 'build' / 'make.log', 'w') as log:
        ok = subprocess.run(['./make.sh', 'board'], cwd=copy, env=env, stdout=log,
                            stderr=subprocess.STDOUT).returncode == 0
    result = {'set': seed_set, 'ok': ok, 'seconds': round(time.time() - start)}
    if ok:
        out = subprocess.run([KPY, 'tools/board_metrics.py', 'kicad/voicestick-c3-v1.kicad_pcb'],
                             cwd=copy, capture_output=True, text=True, check=True).stdout
        result.update(json.loads(out.strip().splitlines()[-1]))
    else:
        result['log'] = str(copy / 'build' / 'make.log')
    print(f"  set {seed_set:3d}: {'ok' if ok else 'FAILED'} in {result['seconds']} s", flush=True)
    return result


def rank_key(r):
    if not r['ok']:
        return (1,)
    return (0, r['narrow_power_mm'], r['signal_vias'], r['signal_length_mm'], -r['gnd_outer_area_mm2'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sets', default='0-9')
    parser.add_argument('--jobs', type=int, default=5)
    args = parser.parse_args()
    sets = parse_sets(args.sets)
    WORK.mkdir(exist_ok=True)
    print(f'routing {len(sets)} seed sets, {args.jobs} at a time')
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = sorted(pool.map(run_set, sets), key=rank_key)

    (ROOT / 'build').mkdir(exist_ok=True)
    (ROOT / 'build' / 'seed_search.json').write_text(json.dumps(results, indent=1))
    print(f"\n{'set':>4} {'ok':>3} {'narrow':>7} {'sig vias':>8} {'sig mm':>7} {'gnd vias':>8} "
          f"{'gnd mm2':>8} {'islands':>7}")
    for r in results:
        if r['ok']:
            print(f"{r['set']:4d} {'yes':>3} {r['narrow_power_mm']:7.2f} {r['signal_vias']:8d} "
                  f"{r['signal_length_mm']:7.1f} {r['gnd_vias']:8d} {r['gnd_outer_area_mm2']:8.1f} "
                  f"{r['gnd_outer_islands']:7d}")
        else:
            print(f"{r['set']:4d}  no  see {r['log']}")


if __name__ == '__main__':
    main()
