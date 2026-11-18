#!/usr/bin/env python3
"""Draw the top-view orientation map from assembly-interface.json.

The drawing uses the user's holding view: main-key end up, USB end down, so
-Y appears on the left and +Y on the right.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
S = 9.0  # px per mm


def to_px(x, y):
    # Holding view: mechanical -X is up, +Y is right.
    return 220 + y * S, 330 + x * S


def rect(x0, x1, y0, y1, fill, stroke='#333', label='', opacity=1.0):
    (ax, ay), (bx, by) = to_px(x0, y0), to_px(x1, y1)
    left, top = min(ax, bx), min(ay, by)
    w, h = abs(bx - ax), abs(by - ay)
    out = (f'<rect x="{left:.1f}" y="{top:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{fill}" '
           f'fill-opacity="{opacity}" stroke="{stroke}" stroke-width="1"/>')
    if label:
        out += (f'<text x="{left + w / 2:.1f}" y="{top + h / 2 + 4:.1f}" font-size="11" '
                f'text-anchor="middle" font-family="Helvetica">{label}</text>')
    return out


def circle(x, y, r, fill, label=''):
    cx, cy = to_px(x, y)
    out = f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r * S:.1f}" fill="{fill}" stroke="#333"/>'
    if label:
        out += f'<text x="{cx + r * S + 3:.1f}" y="{cy + 4:.1f}" font-size="11" font-family="Helvetica">{label}</text>'
    return out


def main():
    i = json.loads((ROOT / 'assembly-interface.json').read_text())
    bw, bh = i['board']['outline_xy']
    cw, ch = i['case']['outer_xy']
    parts = [rect(-cw / 2, cw / 2, -ch / 2, ch / 2, '#f4f1ea', label=''),
             rect(-bw / 2, bw / 2, -bh / 2, bh / 2, '#2f6b3a', opacity=0.35)]
    m = i['main_key']
    mx, my = m['center_xy']
    parts.append(rect(mx - m['body_xy'][0] / 2, mx + m['body_xy'][0] / 2, my - m['body_xy'][1] / 2,
                      my + m['body_xy'][1] / 2, '#3b5fc0', label='MAIN KEY SW1', opacity=0.6))
    names = {'previous': 'PREV SW2', 'next': 'NEXT SW3', 'cancel_boot': 'CANCEL/BOOT SW4'}
    for k in i['side_keys']['keys']:
        y = k['side'] * 15.6
        parts.append(rect(k['x'] - 3, k['x'] + 3, y - 1.2, y + 1.2, '#c8352b', label=''))
        tx, ty = to_px(k['x'], k['side'] * 19.5)
        parts.append(f'<text x="{tx:.1f}" y="{ty + 4:.1f}" font-size="12" text-anchor="middle" '
                     f'font-family="Helvetica" transform="rotate(-90 {tx:.1f} {ty:.1f})">{names[k["role"]]}</text>')
    u = i['usb']
    parts.append(rect(u['origin_xy'][0] - 3.65, u['front_x'], -4.47, 4.47, '#999', label='USB-C'))
    a = i['antenna']
    parts.append(rect(*a['copper_keepout_x'], *a['copper_keepout_y'], '#ffd84d', label='ANT', opacity=0.7))
    b = i['battery']
    bx, by = b['center_xy']
    ex, ey, _ = b['envelope_xyz']
    parts.append(rect(bx - ex / 2, bx + ex / 2, by - ey / 2, by + ey / 2, 'none', stroke='#a33', label='battery (above)'))
    mic = i['microphone']
    parts.append(circle(*mic['port_xy'], 0.8, '#000', 'MIC port'))
    for led in i['light_window']['leds']:
        parts.append(circle(*led['xy'], 1.2, '#ffef9e', f"{led['ref']} window"))
    for h in i['fasteners']['holes']:
        parts.append(circle(*h['xy'], 1.1, '#fff', h['ref']))
    ax, ay = to_px(0, 0)
    arrows = (f'<g font-family="Helvetica" font-size="13">'
              f'<text x="20" y="30">VoiceStick C3 v1 top view (holding view)</text>'
              f'<text x="20" y="50">main-key end up, USB end down; -Y LEFT, +Y RIGHT</text>'
              f'<line x1="{ax}" y1="{ay}" x2="{ax}" y2="{ay + 60}" stroke="#000" marker-end="url(#a)"/>'
              f'<text x="{ax + 4}" y="{ay + 75}">+X</text>'
              f'<line x1="{ax}" y1="{ay}" x2="{ax + 60}" y2="{ay}" stroke="#000" marker-end="url(#a)"/>'
              f'<text x="{ax + 64}" y="{ay + 4}">+Y</text></g>')
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="440" height="620" viewBox="0 0 440 620">'
           '<defs><marker id="a" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto">'
           '<path d="M0,0 L6,3 L0,6 z"/></marker></defs><rect width="440" height="620" fill="#fff"/>'
           + ''.join(parts) + arrows + '</svg>')
    out = ROOT / 'orientation-top.svg'
    out.write_text(svg)
    print('orientation map:', out)


if __name__ == '__main__':
    main()
