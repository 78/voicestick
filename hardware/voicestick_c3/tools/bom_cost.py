#!/usr/bin/env python3
"""Price the assembly BOM with the JLCPCB parts library (USD, overseas prices).

    python3 tools/bom_cost.py [--boards 5,50,1000] [--rate 7.1]

For each board count it prints the parts cost the way JLC bills an assembly
order: every line is bought as max(qty x boards + attrition, minimum quantity)
at the matching price break. Assembly fees (setup, stencil, extended-part
loading, solder joints) are not included; read them off the JLC quote.
"""
import argparse
import csv
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOM = ROOT / 'fab' / 'voicestick-c3-v1-bom.csv'
URL = 'https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList'


def lookup(code):
    body = json.dumps({'keyword': code, 'currentPage': 1, 'pageSize': 5}).encode()
    req = urllib.request.Request(URL, body, {'Content-Type': 'application/json'})
    parts = json.load(urllib.request.urlopen(req, timeout=30))['data']['componentPageInfo']['list'] or []
    return next(p for p in parts if p['componentCode'] == code)


def unit_price(part, count):
    price = part['componentPrices'][0]['productPrice']
    for tier in sorted(part['componentPrices'], key=lambda t: t['startNumber']):
        if count >= tier['startNumber']:
            price = tier['productPrice']
    return price


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--boards', default='5,50,1000')
    ap.add_argument('--rate', type=float, default=7.1, help='CNY per USD')
    args = ap.parse_args()
    boards = [int(n) for n in args.boards.split(',')]

    lines = []
    for row in csv.DictReader(BOM.open()):
        part = lookup(row['LCSC Part #'])
        lines.append((row, part))

    totals = {n: 0.0 for n in boards}
    head = ''.join(f'{f"{n} pcs/board":>15}' for n in boards)
    print(f"{'refs':24} {'LCSC':>10} {'lib':>6} {'qty':>3}{head}")
    for row, part in sorted(lines, key=lambda l: -unit_price(l[1], 1) * int(l[0]['Qty'])):
        qty = int(row['Qty'])
        cells = ''
        for n in boards:
            bought = max(qty * n + (part['lossNumber'] or 0), part['leastPatchNumber'] or 0)
            cost = bought * unit_price(part, bought) / n
            totals[n] += cost
            cells += f'{cost:15.4f}'
        lib = 'basic' if part['componentLibraryType'] == 'base' else 'ext'
        print(f"{row['Designator'][:24]:24} {part['componentCode']:>10} {lib:>6} {qty:3d}{cells}")
    print(f"{'parts total (USD/board)':45}" + ''.join(f'{totals[n]:15.2f}' for n in boards))
    print(f"{f'parts total (CNY/board @ {args.rate})':45}" + ''.join(f'{totals[n] * args.rate:15.2f}' for n in boards))
    extended = sum(p['componentLibraryType'] != 'base' for _, p in lines)
    print(f'{len(lines)} lines, {extended} extended (each extended line adds a loading fee per order)')


if __name__ == '__main__':
    main()
