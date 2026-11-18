#!/usr/bin/env python3
"""Generate the v1 schematic from design.py.

The sheet is a grid of framed blocks (the `block` of each part in design.py).
Inside a block, the parts and wires drawn in schematic_layout.py are placed
as given; nets that leave the block carry a global label there. Every pin the
layout does not wire falls back to a short stub ending in a global label, or
in a power symbol for GND / +3V3, and parts without a layout are packed
automatically in that style. Connectivity is therefore always the pin map in
design.py; KiCad ERC and check_netlist.py verify it after `make.sh` exports
the netlist.
"""
import uuid
from collections import Counter
from pathlib import Path

import design
import schematic_layout
from sexpr import Q, child, children, dump, read_library_symbol, symbol_pins

ROOT = Path(__file__).resolve().parents[1]
KICAD = ROOT / 'kicad'
PROJECT = 'voicestick-c3-v1'
STOCK = Path('/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols')
NS = uuid.UUID('6f0c3b0e-5a2e-4e4c-9a55-3b1d0d6b7a01')
GRID = 1.27
STUB = 2.54
FONT = 1.0
LABEL_CHAR = 0.9

# Rows of framed blocks on the sheet, left to right.
BLOCKS = [
    [('usb', 'USB-C input + ESD'), ('charge', 'Charger + charge LED'),
     ('path', 'Power path: USB / battery -> VSYS'), ('ldo', '3.3 V LDO'), ('sense', 'Battery / VBUS sensing')],
    [('mcu', 'ESP32-C3 + decoupling + EN'), ('flash', '8 MB SPI flash'),
     ('clock', '40 MHz / 32.768 kHz crystals'), ('rf', 'Antenna pi match (tune with VNA)')],
    [('periph', 'Switched peripheral rail: WS2812 + PDM mic'), ('keys', 'Keys'), ('test', 'Test points'),
     ('mech', 'Mounting holes (M2)'), ('flags', 'Power flags')],
]
POWER_SYMBOLS = {'GND': 'power:GND', '+3V3': 'power:+3V3'}
# Stub direction (sheet coords, y down) -> rotation that points the symbol body along it.
POWER_ROTATION = {
    'power:GND': {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270},
    'power:+3V3': {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270},
}
TWO_PIN = ('Device:R_Small', 'Device:C_Small', 'Device:L_Small')
DIRECTIONS = {'left': (-1, 0), 'right': (1, 0), 'up': (0, -1), 'down': (0, 1)}
FRAME_PAD, TITLE_H, BLOCK_GAP, PAGE_MARGIN, TITLE_BLOCK_H = 3.81, 6.35, 5.08, 17.78, 35.0


def uid(*parts):
    return str(uuid.uuid5(NS, '/'.join(str(p) for p in parts)))


def snap(v):
    return round(round(v / GRID) * GRID, 3)


def pt(x, y):
    return (snap(x), snap(y))


def load_symbol(lib_id):
    lib, name = lib_id.split(':')
    path = KICAD / f'{lib}.kicad_sym' if lib == design.LIB else STOCK / f'{lib}.kicad_sym'
    sym = list(read_library_symbol(path, name))
    sym[1] = Q(lib_id)
    return sym


def bbox(sym):
    xs, ys = [0.0], [0.0]
    for unit in children(sym, 'symbol'):
        for item in unit:
            if not isinstance(item, list):
                continue
            for key in ('start', 'end', 'at', 'center'):
                p = child(item, key)
                if p and item[0] != 'pin':
                    xs.append(float(p[1]))
                    ys.append(float(p[2]))
            for pts in children(item, 'pts'):
                for xy in children(pts, 'xy'):
                    xs.append(float(xy[1]))
                    ys.append(float(xy[2]))
    for x, y, *_ in symbol_pins(sym).values():
        xs.append(x)
        ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)


def rotate(px, py, angle):
    """Rotate a symbol-space point (y up) counter-clockwise by a multiple of 90 degrees."""
    c, s = {0: (1, 0), 90: (0, 1), 180: (-1, 0), 270: (0, -1)}[int(angle) % 360]
    return px * c - py * s, px * s + py * c


def outward(angle):
    """Sheet-space unit vector pointing away from the body for a pin angle."""
    return {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}[int(angle) % 360]


def label_angle(direction):
    return {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[direction]


def passive_field(name, rot):
    """Reference / value offset for small two-pin parts: beside the body when
    vertical, above and below it when horizontal."""
    if rot in (90, 270):
        return 0.0, 2.032 if name == 'Reference' else -2.032, []
    return 1.524, 0.762 if name == 'Reference' else -1.27, ['left']


def effects(size=FONT, hide=False, justify=None):
    node = ['effects', ['font', ['size', size, size]]]
    if justify:
        node.append(['justify'] + justify)
    if hide:
        node.append(['hide', 'yes'])
    return node


def on_segment(p, a, b):
    """True if p lies strictly inside the axis-aligned segment a-b."""
    if p in (a, b):
        return False
    if a[0] == b[0] == p[0]:
        return min(a[1], b[1]) < p[1] < max(a[1], b[1])
    if a[1] == b[1] == p[1]:
        return min(a[0], b[0]) < p[0] < max(a[0], b[0])
    return False


def orthogonal(points):
    """Insert a horizontal-first corner between points that are not aligned."""
    out = [points[0]]
    for p in points[1:]:
        q = out[-1]
        if q[0] != p[0] and q[1] != p[1]:
            out.append((p[0], q[1]))
        out.append(p)
    return out


def shift(node, dx, dy):
    """Move every coordinate of an s-expression item by (dx, dy)."""
    if not isinstance(node, list):
        return node
    if node and node[0] in ('at', 'xy', 'start', 'end') and len(node) >= 3:
        return [node[0], round(node[1] + dx, 3), round(node[2] + dy, 3)] + node[3:]
    return [shift(n, dx, dy) for n in node]


class Sheet:
    """Items of one block, drawn in block-local coordinates (mm, y down)."""
    power_count = 0

    def __init__(self, name):
        self.name = name
        self.items = []
        self.lib = {}
        self.points = []
        self.pins = []
        self.taps = []
        self.ends = set()
        self.segments = []

    def symbol(self, p, x, y, rot=0, fields=None):
        """Place a part; returns {pin number: (point, outward direction)}.
        `fields` optionally moves Reference / Value to (dx, dy) from the part, left-justified."""
        ref, lib_id = p['ref'], p['lib_id']
        sym = load_symbol(lib_id)
        self.lib.setdefault(lib_id, sym)
        is_power = lib_id.startswith('power:')
        props = []
        for prop in children(sym, 'property'):
            name = str(prop[1])
            if name not in ('Reference', 'Value', 'Footprint', 'Datasheet', 'Description'):
                continue
            value = {'Reference': ref, 'Value': p['value'], 'Footprint': p.get('footprint', '')}.get(
                name, str(prop[2]))
            at = child(prop, 'at')
            hidden = (child(prop, 'hide') is not None or ref.startswith('#') and name == 'Reference'
                      or name in ('Footprint', 'Datasheet', 'Description'))
            justify = list((child(child(prop, 'effects'), 'justify') or ['justify'])[1:])
            if fields and name in fields:
                fx, fy, justify = fields[name][0], -fields[name][1], ['left']
            elif lib_id in TWO_PIN and name in ('Reference', 'Value'):
                fx, fy, justify = passive_field(name, rot)
            else:
                fx, fy = rotate(float(at[1]), float(at[2]), rot)
            props.append(['property', Q(name), Q(value), ['at', round(x + fx, 3), round(y - fy, 3), rot % 180],
                          effects(hide=hidden, justify=justify or None)])
            if not hidden:
                w = len(value) * LABEL_CHAR
                left = {'left': 0, 'right': -w}.get(next((j for j in justify if j in ('left', 'right')), ''), -w / 2)
                self.points += [(x + fx + left, y - fy - 0.8), (x + fx + left + w, y - fy + 0.8)]
        for key in ('LCSC', 'MPN', 'Note'):
            value = p.get(key.lower(), '')
            if value:
                props.append(['property', Q(key), Q(value), ['at', x, y, 0], effects(hide=True)])
        node = ['symbol', ['lib_id', Q(lib_id)], ['at', x, y, rot], ['unit', 1],
                ['exclude_from_sim', 'no'],
                ['in_bom', 'no' if is_power or p['block'] in ('test', 'mech') else 'yes'],
                ['on_board', 'no' if is_power else 'yes'], ['dnp', 'yes' if p.get('dnp') else 'no'],
                ['uuid', uid('sym', ref)]] + props
        pins = {}
        for number, (px, py, angle, _name, _kind) in symbol_pins(sym).items():
            node.append(['pin', Q(number), ['uuid', uid('pin', ref, number)]])
            rx, ry = rotate(px, py, rot)
            at = pt(x + rx, y - ry)
            pins[number] = (at, outward(angle + rot))
            self.pins.append(at)
        node.append(['instances', ['project', Q(PROJECT),
                                   ['path', Q('/' + uid('root')), ['reference', Q(ref)], ['unit', 1]]]])
        self.items.append(node)
        x0, y0, x1, y1 = bbox(sym)
        for cx, cy in ((x0, y0), (x1, y1), (x0, y1), (x1, y0)):
            rx, ry = rotate(cx, cy, rot)
            self.points.append((x + rx, y - ry))
        return pins

    def wire(self, points):
        for a, b in zip(points, points[1:]):
            self.segments.append((a, b))
            self.points.extend((a, b))

    def label(self, net, at, direction):
        self.items.append(['global_label', Q(net), ['shape', 'passive'],
                           ['at', at[0], at[1], label_angle(direction)], ['fields_autoplaced', 'yes'],
                           effects(justify=['left' if direction in ((1, 0), (0, -1)) else 'right']),
                           ['uuid', uid('label', self.name, net, *at)]])
        length = len(net) * LABEL_CHAR + 3.0
        self.points.extend((at, (at[0] + direction[0] * length, at[1] + direction[1] * length)))

    def net_name(self, net, at):
        """Name a net that stays inside the block. A global label keeps the plain
        design.py net name; local labels would be exported as '/NAME'."""
        self.items.append(['global_label', Q(net), ['shape', 'passive'], ['at', at[0], at[1], 0],
                           ['fields_autoplaced', 'yes'], effects(justify=['left']),
                           ['uuid', uid('name', self.name, net, *at)]])
        self.taps.append(at)
        self.points.extend((at, (at[0] + len(net) * LABEL_CHAR, at[1] - 1.5)))

    def power(self, net, at, direction=None):
        lib_id = POWER_SYMBOLS[net]
        direction = direction or ((0, 1) if net == 'GND' else (0, -1))
        Sheet.power_count += 1
        ref = f'#PWR{Sheet.power_count:03d}'
        self.symbol({'ref': ref, 'lib_id': lib_id, 'value': net, 'block': 'power', 'pins': {}},
                    at[0], at[1], POWER_ROTATION[lib_id][direction])

    def flag(self, net, at):
        number = design.POWER_FLAGS.index(net) + 1
        self.symbol({'ref': f'#FLG{number:02d}', 'lib_id': 'power:PWR_FLAG', 'value': 'PWR_FLAG',
                     'block': 'power', 'pins': {}}, at[0], at[1])

    def no_connect(self, at):
        self.items.append(['no_connect', ['at', at[0], at[1]], ['uuid', uid('nc', self.name, *at)]])

    def terminate(self, net, at, direction):
        """Stub from a pin that ends in a power symbol or a global label."""
        if at in self.ends:
            return
        self.ends.add(at)
        if net is None:
            self.no_connect(at)
            return
        end = pt(at[0] + direction[0] * STUB, at[1] + direction[1] * STUB)
        self.wire([at, end])
        if net in POWER_SYMBOLS:
            self.power(net, end, direction)
        else:
            self.label(net, end, direction)

    def text(self, value, at):
        self.items.append(['text', Q(value), ['exclude_from_sim', 'no'], ['at', at[0], at[1], 0],
                           effects(justify=['left']), ['uuid', uid('text', self.name, value)]])
        self.points.extend((at, (at[0] + len(value) * LABEL_CHAR, at[1] + 1.0)))

    def finish_wires(self):
        """Emit wires broken at every T joint, with junction dots where 3+ ends meet."""
        segments = [(a, b) for a, b in self.segments if a != b]
        nodes = set(self.pins) | set(self.taps) | {p for s in segments for p in s}
        pieces = []
        for a, b in segments:
            inner = sorted((n for n in nodes if on_segment(n, a, b)),
                           key=lambda n: abs(n[0] - a[0]) + abs(n[1] - a[1]))
            chain = [a] + inner + [b]
            pieces += list(zip(chain, chain[1:]))
        ends = Counter(p for piece in pieces for p in piece)
        pins = set(self.pins)
        for p, n in sorted(ends.items()):
            if n + (p in pins) >= 3:
                self.items.append(['junction', ['at', p[0], p[1]], ['diameter', 0], ['color', 0, 0, 0, 0],
                                   ['uuid', uid('junction', self.name, *p)]])
        for a, b in sorted(set(pieces)):
            self.items.append(['wire', ['pts', ['xy', a[0], a[1]], ['xy', b[0], b[1]]],
                               ['stroke', ['width', 0], ['type', 'default']],
                               ['uuid', uid('wire', self.name, *a, *b)]])

    def extent(self):
        xs, ys = [p[0] for p in self.points], [p[1] for p in self.points]
        return min(xs), min(ys), max(xs), max(ys)


def auto_extent(p, sym):
    """Room a part needs with stubs and labels (sheet coords, y down)."""
    x0, y0, x1, y1 = bbox(sym)
    grow = {(-1, 0): 0, (1, 0): 0, (0, -1): 0, (0, 1): 0}
    for number, (*_, angle, _n, _k) in symbol_pins(sym).items():
        net = p['pins'].get(number) or ''
        size = 5.0 if net in POWER_SYMBOLS else len(net) * LABEL_CHAR + 3.5
        grow[outward(angle)] = max(grow[outward(angle)], STUB + size)
    if p['lib_id'] == 'Connector:TestPoint':
        x0, x1 = x0 - 3.0, x1 + 3.0
    return x0 - grow[(-1, 0)], -y1 - grow[(0, -1)], x1 + grow[(1, 0)], -y0 + grow[(0, 1)]


def auto_place(sheet, parts, top, max_width=150.0, gap=5.08):
    """Pack parts in rows, each pin with a stub and a label or power symbol."""
    x, y, row_h = 0.0, top, 0.0
    for p in parts:
        sym = load_symbol(p['lib_id'])
        left, t, right, b = auto_extent(p, sym)
        w, h = right - left, b - t
        if x > 0 and x + w > max_width:
            x, y, row_h = 0.0, y + row_h + gap, 0.0
        pins = sheet.symbol(p, snap(x - left), snap(y - t))
        for number, (at, direction) in pins.items():
            sheet.terminate(p['pins'].get(number), at, direction)
        x += w + gap
        row_h = max(row_h, h)


def build_block(block):
    sheet = Sheet(block)
    layout = schematic_layout.LAYOUTS.get(block, {})
    placed = layout.get('parts', {})
    fields = layout.get('fields', {})
    pins = {ref: sheet.symbol(design.PARTS[ref], *spec, fields=fields.get(ref)) for ref, spec in placed.items()}
    wired = set()
    for path in layout.get('wires', []):
        points = []
        for item in path:
            if isinstance(item, str):
                ref, number = item.rsplit('.', 1)
                at = pins[ref][number][0]
                points.append(at)
                wired.update((ref, n) for n, (other, _) in pins[ref].items() if other == at)
            else:
                points.append(pt(*item))
        sheet.wire(orthogonal(points))
    for net, at, direction in layout.get('labels', []):
        sheet.label(net, pt(*at), DIRECTIONS[direction])
    for net, at in layout.get('names', []):
        sheet.net_name(net, pt(*at))
    for net, at, *direction in layout.get('power', []):
        sheet.power(net, pt(*at), DIRECTIONS[direction[0]] if direction else None)
    for net, at in layout.get('flags', []):
        sheet.flag(net, pt(*at))
    for value, at in layout.get('texts', []):
        sheet.text(value, pt(*at))
    for ref in placed:
        for number, (at, direction) in pins[ref].items():
            if (ref, number) not in wired:
                sheet.terminate(design.PARTS[ref]['pins'].get(number), at, direction)

    if block == 'flags':
        inline = {net for spec in schematic_layout.LAYOUTS.values() for net, _ in spec.get('flags', [])}
        rest = [{'ref': f'#FLG{i + 1:02d}', 'lib_id': 'power:PWR_FLAG', 'value': 'PWR_FLAG',
                 'block': 'power', 'pins': {'1': net}}
                for i, net in enumerate(design.POWER_FLAGS) if net not in inline]
    else:
        rest = [p for p in design.PARTS.values() if p['block'] == block and p['ref'] not in placed]
    if rest:
        top = sheet.extent()[3] + 5.08 if sheet.points else 0.0
        auto_place(sheet, rest, top)
    sheet.finish_wires()
    return sheet


def compose(rows, page=None):
    """Lay out rows of framed blocks; frames in a row share its height and are
    stretched so that the rows fill `page` (w, h) above the title block, or the
    widest row. Each block is centred in its frame."""
    sized = []
    for row in rows:
        sized.append([])
        for name, title, sheet in row:
            x0, y0, x1, y1 = sheet.extent()
            w = max(x1 - x0, len(title) * 1.6) + 2 * FRAME_PAD
            sized[-1].append((name, title, sheet, w, y1 - y0 + 2 * FRAME_PAD + TITLE_H))
    content_w = max(sum(r[3] for r in row) + BLOCK_GAP * (len(row) - 1) for row in sized)
    row_heights = [max(r[4] for r in row) for row in sized]
    grow = 1.0
    if page:
        content_w = max(content_w, page[0] - 2 * PAGE_MARGIN)
        free_h = page[1] - 2 * PAGE_MARGIN - TITLE_BLOCK_H - BLOCK_GAP * (len(sized) - 1)
        grow = max(1.0, free_h / sum(row_heights))

    items, lib, y = [], {}, PAGE_MARGIN
    for row, row_h in zip(sized, row_heights):
        row_h *= grow
        stretch = (content_w - BLOCK_GAP * (len(row) - 1)) / sum(r[3] for r in row)
        x = PAGE_MARGIN
        for name, title, sheet, natural_w, h in row:
            w = natural_w * stretch
            x0, y0, _, _ = sheet.extent()
            dx = snap(x + (w - natural_w) / 2 + FRAME_PAD - x0)
            dy = snap(y + (row_h - h) / 2 + FRAME_PAD + TITLE_H - y0)
            items += [shift(item, dx, dy) for item in sheet.items]
            lib.update(sheet.lib)
            items.append(['rectangle', ['start', snap(x), snap(y)], ['end', snap(x + w), snap(y + row_h)],
                          ['stroke', ['width', 0.254], ['type', 'default'], ['color', 72, 72, 160, 1]],
                          ['fill', ['type', 'none']], ['uuid', uid('frame', name)]])
            items.append(['text', Q(title), ['exclude_from_sim', 'no'],
                          ['at', snap(x + FRAME_PAD), snap(y + FRAME_PAD + 1.27), 0],
                          effects(size=2.0, justify=['left']), ['uuid', uid('title', name)]])
            x += w + BLOCK_GAP
        y += row_h + BLOCK_GAP
    return items, lib, content_w + 2 * PAGE_MARGIN, y + PAGE_MARGIN


def write(path, paper, items, lib):
    root = ['kicad_sch', ['version', 20250114], ['generator', Q('eeschema')],
            ['generator_version', Q('10.0')], ['uuid', uid('root')], ['paper', Q(paper)],
            ['title_block', ['title', Q('VoiceStick C3 v1')], ['rev', Q('1')],
             ['company', Q('VoiceStick')],
             ['comment', 1, Q('Generated by hardware/voicestick_c3/tools/build_schematic.py from design.py')]],
            ['lib_symbols'] + list(lib.values())] + items + [
            ['sheet_instances', ['path', Q('/'), ['page', Q('1')]]], ['embedded_fonts', 'no']]
    path.write_text(dump(root) + '\n')


def main():
    rows = [[(name, title, sheet) for name, title in row if (sheet := build_block(name)).items]
            for row in BLOCKS]
    _, _, width, height = compose(rows)
    paper, page = next(((name, size) for name, size in (('A3', (420, 297)), ('A2', (594, 420)),
                                                        ('A1', (841, 594)))
                        if width <= size[0] and height + TITLE_BLOCK_H <= size[1]), ('A0', (1189, 841)))
    items, lib, width, height = compose(rows, page)
    out = KICAD / f'{PROJECT}.kicad_sch'
    write(out, paper, items, lib)
    print(f'schematic written: {out} ({paper}, {width:.0f} x {height:.0f} mm, {len(design.PARTS)} parts)')


if __name__ == '__main__':
    main()
