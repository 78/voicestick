#!/usr/bin/env python3
"""Run kicad-cli DRC and print violations grouped by type.

Usage: drc_summary.py [board.kicad_pcb] [--types a,b] [--all]
Exit code is the number of error-severity violations (capped at 255).
"""
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KICAD_CLI = '/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli'


def mech(pos):
    return f"({pos['x'] - 100:.2f},{100 - pos['y']:.2f})"


def run(board, refill=False):
    report = board.with_suffix('.drc.json')
    cmd = [KICAD_CLI, 'pcb', 'drc', '--format', 'json', '--severity-all', '--schematic-parity',
           '-o', str(report), str(board)]
    if refill:
        cmd.insert(3, '--refill-zones')
    subprocess.run(cmd, check=True, capture_output=True)
    return json.loads(report.read_text())


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    board = Path(args[0]) if args else ROOT / 'kicad' / 'voicestick-c3-v1.kicad_pcb'
    only = None
    for a in sys.argv[1:]:
        if a.startswith('--types='):
            only = set(a.split('=', 1)[1].split(','))
    data = run(board, refill='--refill' in sys.argv)
    items = []
    for section in ('violations', 'unconnected_items', 'schematic_parity'):
        for v in data.get(section, []):
            items.append((section, v))
    counts = Counter((v['severity'], v['type']) for _, v in items)
    for (sev, kind), n in sorted(counts.items()):
        print(f'{sev:8s} {kind:32s} {n}')
    show_all = '--all' in sys.argv
    for section, v in items:
        if only and v['type'] not in only:
            continue
        if not show_all and not only and v['severity'] != 'error':
            continue
        where = ' | '.join(f"{it['description']} {mech(it['pos'])}" for it in v.get('items', []))
        print(f"  [{v['type']}] {v['description']}: {where}")
    errors = sum(n for (sev, _), n in counts.items() if sev == 'error')
    print(f'total {len(items)} items, {errors} errors')
    return min(errors, 255)


if __name__ == '__main__':
    sys.exit(main())
