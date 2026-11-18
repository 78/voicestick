#!/usr/bin/env python3
"""Compare the KiCad-exported netlist against design.py pin by pin."""
import sys
from pathlib import Path

import design
from sexpr import child, children, parse

ROOT = Path(__file__).resolve().parents[1]


def read_netlist(path):
    root = parse(path.read_text())
    result = {}
    for net in children(child(root, 'nets'), 'net'):
        name = str(child(net, 'name')[1]).lstrip('/')
        nodes = {(str(child(n, 'ref')[1]), str(child(n, 'pin')[1])) for n in children(net, 'node')}
        if name.startswith('unconnected-') or name.startswith('Net-('):
            continue
        result[name] = nodes
    return result


def main():
    exported = read_netlist(ROOT / 'kicad' / 'voicestick-c3-v1.net')
    expected = {n: set(nodes) for n, nodes in design.nets().items()}
    problems = []
    for name in sorted(set(exported) | set(expected)):
        a, b = exported.get(name, set()), expected.get(name, set())
        a = {x for x in a if not x[0].startswith('#')}
        if a != b:
            problems.append(f'{name}: missing {sorted(b - a)} extra {sorted(a - b)}')
    for line in problems:
        print('MISMATCH', line)
    print(f'netlist parity: {len(expected)} nets, {sum(len(v) for v in expected.values())} pins, '
          f'{len(problems)} mismatches')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
