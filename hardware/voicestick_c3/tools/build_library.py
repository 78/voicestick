#!/usr/bin/env python3
"""Generate the v1 project symbol and footprint libraries.

Only parts without a usable KiCad stock symbol/footprint live here. Land
patterns cite the datasheet drawing they were taken from.
"""
from pathlib import Path

from sexpr import Q, dump

ROOT = Path(__file__).resolve().parents[1]
KICAD = ROOT / 'kicad'
LIB = 'VoiceStickC3V1'
FONT = ['effects', ['font', ['size', 1.27, 1.27]]]


def prop(name, value, x=0.0, y=0.0, hide=False):
    node = ['property', Q(name), Q(value), ['at', x, y, 0]]
    node.append(['effects', ['font', ['size', 1.27, 1.27]]] + ([['hide', 'yes']] if hide else []))
    return node


def box_symbol(name, value, footprint, description, left, right, width=12.7):
    """Rectangle symbol; `left`/`right` are lists of (number, name, type) top-down."""
    rows = max(len(left), len(right))
    half_h = (rows + 1) * 2.54 / 2
    half_h = round(half_h / 1.27) * 1.27
    pins = []
    for side, entries in ((-1, left), (1, right)):
        for i, (number, pin_name, kind) in enumerate(entries):
            y = half_h - 2.54 * (i + 1)
            x = side * (width / 2 + 2.54)
            pins.append(['pin', kind, 'line', ['at', x, y, 0 if side < 0 else 180], ['length', 2.54],
                         ['name', Q(pin_name), FONT], ['number', Q(number), FONT]])
    return ['symbol', Q(name),
            ['exclude_from_sim', 'no'], ['in_bom', 'yes'], ['on_board', 'yes'],
            prop('Reference', 'U', 0, half_h + 1.27),
            prop('Value', value, 0, -half_h - 1.27),
            prop('Footprint', footprint, hide=True),
            prop('Datasheet', '', hide=True),
            prop('Description', description, hide=True),
            ['symbol', Q(f'{name}_0_1'),
             ['rectangle', ['start', -width / 2, half_h], ['end', width / 2, -half_h],
              ['stroke', ['width', 0.254], ['type', 'default']], ['fill', ['type', 'background']]]],
            ['symbol', Q(f'{name}_1_1')] + pins,
            ['embedded_fonts', 'no']]


SYMBOLS = [
    box_symbol('TP4054', 'TP4054', 'Package_TO_SOT_SMD:SOT-23-5',
               'Linear Li-ion charger, open-drain CHRG, I_BAT = 1000 V / R_PROG',
               [('4', 'VCC', 'power_in'), ('5', 'PROG', 'passive'), ('2', 'GND', 'power_in')],
               [('3', 'BAT', 'power_out'), ('1', '~{CHRG}', 'open_collector')]),
    box_symbol('RT9080-33', 'RT9080-33GJ5', 'Package_TO_SOT_SMD:SOT-23-5',
               '3.3 V 600 mA LDO, 2 uA quiescent current',
               [('1', 'VIN', 'power_in'), ('3', 'EN', 'input'), ('2', 'GND', 'power_in')],
               [('5', 'VOUT', 'power_out'), ('4', 'NC', 'no_connect')]),
    box_symbol('SD18OB371-041', 'SD18OB371-041', f'{LIB}:Goertek_SD18OB371-041',
               'Goertek bottom-port PDM MEMS microphone, 1/2-cycle PDM, 1.62-3.6 V',
               [('5', 'VDD', 'passive'), ('4', 'CLK', 'input'), ('2', 'L/R', 'input')],
               [('1', 'DATA', 'output'), ('3', 'GND', 'passive')]),
]


def write_symbols():
    lib = ['kicad_symbol_lib', ['version', 20251024], ['generator', Q('voicestick_c3_v1')],
           ['generator_version', Q('10.0')]] + SYMBOLS
    (KICAD / f'{LIB}.kicad_sym').write_text(dump(lib) + '\n')


def text(kind, value, x, y, layer):
    return ['property', Q(kind), Q(value), ['at', x, y, 0], ['layer', Q(layer)],
            ['effects', ['font', ['size', 0.8, 0.8], ['thickness', 0.12]]]]


def rect(x0, y0, x1, y1, layer, width=0.1):
    return ['fp_rect', ['start', x0, y0], ['end', x1, y1],
            ['stroke', ['width', width], ['type', 'default']], ['fill', 'no'], ['layer', Q(layer)]]


def smd(number, x, y, w, h, shape='roundrect'):
    pad = ['pad', Q(number), 'smd', shape, ['at', x, y], ['size', w, h],
           ['layers', Q('F.Cu'), Q('F.Mask'), Q('F.Paste')]]
    if shape == 'roundrect':
        pad.append(['roundrect_rratio', 0.2])
    return pad


def footprint(name, descr, attr, items, body, model_offset=(0, 0, 0), courtyard=None):
    """Footprint with its STEP model kicad/3dmodels/<name>.step.

    model_offset is in KiCad's 3D frame (mm, Y up, Z from the board top). The
    courtyard defaults to the body outline plus 0.25 mm.
    """
    x0, y0, x1, y1 = body
    cx0, cy0, cx1, cy1 = courtyard or (x0 - 0.25, y0 - 0.25, x1 + 0.25, y1 + 0.25)
    node = ['footprint', Q(name), ['version', 20260206], ['generator', Q('voicestick_c3_v1')],
            ['layer', Q('F.Cu')], ['descr', Q(descr)],
            text('Reference', 'REF**', 0, y0 - 1.0, 'F.SilkS'),
            text('Value', name, 0, y1 + 1.0, 'F.Fab'),
            ['attr', attr],
            rect(x0, y0, x1, y1, 'F.Fab'),
            rect(cx0, cy0, cx1, cy1, 'F.CrtYd', 0.05)] + items
    node.append(['model', Q(f'${{KIPRJMOD}}/3dmodels/{name}.step'), ['offset', ['xyz', *model_offset]],
                 ['scale', ['xyz', 1, 1, 1]], ['rotate', ['xyz', 0, 0, 0]]])
    node.append(['embedded_fonts', 'no'])
    return node


def mic_footprint():
    # Goertek SD18OB371-041 spec v4.0 sections 6.1 and 10.1. Origin = acoustic
    # port centre. Pads mirrored from the bottom view: pin 1 is at -X (top view).
    pad_w, pad_h, col = 0.725, 0.522, 0.475 + 0.725 / 2
    near_row = -1.513 + pad_h / 2          # pins 4/2: top edge 1.513 above the port
    far_row = near_row - pad_h - 0.300     # pins 5/1
    items = [smd('1', -col, far_row, pad_w, pad_h), smd('2', -col, near_row, pad_w, pad_h),
             smd('5', col, far_row, pad_w, pad_h), smd('4', col, near_row, pad_w, pad_h)]
    # Ground ring (ID 1.025 / OD 1.625) as a custom pad anchored on the ring.
    ring_r = (1.025 + 1.625) / 4
    items.append(['pad', Q('3'), 'smd', 'custom', ['at', ring_r, 0], ['size', 0.3, 0.3],
                  ['layers', Q('F.Cu'), Q('F.Mask'), Q('F.Paste')],
                  ['options', ['clearance', 'outline'], ['anchor', 'circle']],
                  ['primitives', ['gr_circle', ['center', -ring_r, 0], ['end', 0, 0],
                                  ['width', 0.3], ['fill', 'no']]]])
    # PCB acoustic hole, unplated, inside the ring.
    items.append(['pad', Q(''), 'np_thru_hole', 'circle', ['at', 0, 0], ['size', 0.6, 0.6],
                  ['drill', 0.6], ['layers', Q('*.Cu'), Q('*.Mask')]])
    items.append(['fp_circle', ['center', -1.6, far_row], ['end', -1.5, far_row],
                  ['stroke', ['width', 0.2], ['type', 'solid']], ['fill', 'yes'], ['layer', Q('F.SilkS')]])
    # Model: EasyEDA RHLGA-5L 3.5x2.65 (LCSC C3040249), origin at the body centre.
    return footprint('Goertek_SD18OB371-041',
                     'Goertek SD18OB371-041 bottom-port PDM mic, 3.50x2.65x0.98; land per spec v4.0 10.1; '
                     'origin at acoustic port; 0.6 mm NPTH sound hole',
                     'smd', items, (-1.325, -2.46, 1.325, 1.04), model_offset=(0, 0.71, 0))


def antenna_footprint():
    # 3216 chip antenna with 0.5 mm end terminations (RFANT3216120A5T class).
    items = [smd('1', -1.45, 0, 0.9, 1.8, 'rect'), smd('2', 1.45, 0, 0.9, 1.8, 'rect'),
             ['fp_line', ['start', -2.2, -1.1], ['end', -2.2, 1.1],
              ['stroke', ['width', 0.15], ['type', 'solid']], ['layer', Q('F.SilkS')]]]
    # Model: EasyEDA 3.2x1.6x1.2 body (LCSC C127629), modelled centred on Z = 0.
    return footprint('ChipAntenna_3216', '2.4 GHz 3.2x1.6 mm chip antenna, pad 1 feed, pad 2 unconnected; '
                     'keep all copper layers clear around it', 'smd', items, (-1.9, -0.9, 1.9, 0.9),
                     model_offset=(0, 0, 0.6))


def main_switch_footprint():
    # Geometry carried over from the rev-a sample footprint (HanElectricity drawing
    # contact offsets 3.80/2.54/5.08); centre bore and drills still need a sample fit.
    items = [['pad', Q(''), 'np_thru_hole', 'circle', ['at', 0, 0], ['size', 4.1, 4.1], ['drill', 4.1],
              ['layers', Q('*.Cu'), Q('*.Mask')]],
             ['pad', Q('1'), 'thru_hole', 'circle', ['at', 2.54, -5.08], ['size', 2.4, 2.4], ['drill', 1.5],
              ['layers', Q('*.Cu'), Q('*.Mask')]],
             ['pad', Q('2'), 'thru_hole', 'circle', ['at', -3.8, -2.54], ['size', 2.4, 2.4], ['drill', 1.5],
              ['layers', Q('*.Cu'), Q('*.Mask')]]]
    # Model: written by enclosure/build_switch_model.py from assembly-interface.json.
    return footprint('CPG151101D05', 'CPG151101D05 PCB-mount mechanical switch; tail 2.85 mm below seating plane',
                     'through_hole', items, (-7.6, -7.85, 7.6, 7.85))


def side_switch_footprint():
    # Alps SKRTLBE010 lands (no guide bosses), checked in rev-a against
    # https://tech.alpsalpine.com/e/products/detail/SKRTLBE010/ . Plunger points +Y.
    items = [smd('1', -1.225, -0.9, 0.75, 1.8), smd('1', 1.225, -0.9, 0.75, 1.8),
             smd('2', 0, -0.9, 0.6, 1.8), smd('MP', -1.85, 1.05, 1.3, 0.9), smd('MP', 1.85, 1.05, 1.3, 0.9),
             rect(-1.0, 1.21, 1.0, 2.04, 'F.Fab')]
    # Model: EasyEDA SKRTLAE010 (LCSC C127481, same body); its plunger tip lands on the F.Fab one.
    return footprint('Alps_SKRTLBE010', 'Alps SKRTLBE010 side-push switch, plunger toward +Y',
                     'smd', items, (-2.25, -1.8, 2.25, 1.5))


def side_led_footprint():
    # Lite-On LTST-S270KRKT, data sheet DS22-2000-210 rev G: body 1.65 x 0.68 x 1.20,
    # lens dome 0.47 beyond the body face towards -Y; suggested lands 1.10 x 1.10
    # with a 0.90 gap. Pad 1 = cathode.
    items = [smd('1', -1.0, 0, 1.1, 1.1), smd('2', 1.0, 0, 1.1, 1.1),
             rect(-0.35, -0.81, 0.35, -0.34, 'F.Fab'),
             ['fp_circle', ['center', -1.0, 0.85], ['end', -0.9, 0.85],
              ['stroke', ['width', 0.2], ['type', 'solid']], ['fill', 'yes'], ['layer', Q('F.SilkS')]]]
    # Model: EasyEDA LED-SMD_L1.7-W0.6-H1.2 (LCSC C125114), origin at the body centre.
    return footprint('LED_LiteOn_LTST-S270', 'Lite-On LTST-S270KRKT side-looking red LED, light towards -Y',
                     'smd', items, (-0.825, -0.34, 0.825, 0.34), courtyard=(-1.8, -1.06, 1.8, 0.8))


def write_footprints():
    out = KICAD / f'{LIB}.pretty'
    out.mkdir(parents=True, exist_ok=True)
    for fp in (mic_footprint(), antenna_footprint(), main_switch_footprint(), side_switch_footprint(),
               side_led_footprint()):
        (out / f'{fp[1]}.kicad_mod').write_text(dump(fp) + '\n')


def write_tables():
    (KICAD / 'sym-lib-table').write_text(
        f'(sym_lib_table\n\t(version 7)\n\t(lib (name "{LIB}") (type "KiCad") '
        f'(uri "${{KIPRJMOD}}/{LIB}.kicad_sym") (options "") (descr "VoiceStick C3 v1 parts"))\n)\n')
    (KICAD / 'fp-lib-table').write_text(
        f'(fp_lib_table\n\t(version 7)\n\t(lib (name "{LIB}") (type "KiCad") '
        f'(uri "${{KIPRJMOD}}/{LIB}.pretty") (options "") (descr "VoiceStick C3 v1 parts"))\n)\n')


if __name__ == '__main__':
    KICAD.mkdir(parents=True, exist_ok=True)
    write_symbols()
    write_footprints()
    write_tables()
    print('library written to', KICAD)
