#!/usr/bin/env python3
"""Write the v1 BOM (JLC assembly format) and the GPIO map from design.py."""
import csv
from collections import OrderedDict
from pathlib import Path

import design

ROOT = Path(__file__).resolve().parents[1]
NOT_ASSEMBLED = ('test', 'mech')


def groups():
    rows = OrderedDict()
    for p in design.PARTS.values():
        if p['dnp'] or p['block'] in NOT_ASSEMBLED:
            continue
        value = p['mpn'] if p['lib_id'].startswith('Switch:') else p['value']
        key = (value, p['footprint'], p['mpn'], p['lcsc'])
        rows.setdefault(key, []).append(p['ref'])
    return rows


def main(out_dir=ROOT):
    path = out_dir / 'bom.csv'
    with path.open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['Comment', 'Designator', 'Footprint', 'MPN', 'LCSC Part #', 'Qty'])
        for (value, footprint, mpn, lcsc), refs in groups().items():
            w.writerow([value, ','.join(refs), footprint.split(':')[1], mpn, lcsc, len(refs)])
    dnp = [p['ref'] for p in design.PARTS.values() if p['dnp']]
    print(f'BOM: {sum(len(r) for r in groups().values())} placements, '
          f'{len(groups())} line items, DNP {dnp} -> {path}')
    return path


if __name__ == '__main__':
    main()
