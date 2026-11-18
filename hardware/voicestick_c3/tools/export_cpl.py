#!/usr/bin/env python3
"""Turn kicad-cli's position CSV into JLC's CPL format.

    python3 tools/export_cpl.py IN_pos.csv OUT_cpl.csv
JLC rotations follow their own footprint library; check every part in the
assembly preview before ordering (see fab/README.md).
"""
import csv
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, newline='') as f:
        rows = list(csv.DictReader(f))
    with open(dst, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Designator', 'Mid X', 'Mid Y', 'Layer', 'Rotation'])
        for r in rows:
            side = 'Top' if r['Side'].lower() == 'top' else 'Bottom'
            w.writerow([r['Ref'], f"{float(r['PosX']):.3f}mm", f"{float(r['PosY']):.3f}mm",
                        side, f"{float(r['Rot']):.0f}"])
    print(f'cpl written: {dst} ({len(rows)} parts)')


if __name__ == '__main__':
    main()
