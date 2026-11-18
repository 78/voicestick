#!/usr/bin/env python3
"""Build the v1 PCB placement with KiCad's pcbnew API.

Run with KiCad's bundled python (see make.sh). Inputs:
  kicad/voicestick-c3-v1.net    netlist exported from the generated schematic
  assembly-interface.json       board outline, mechanical part positions, keepouts
  tools/placement.py            electrical placement
Writes build/01_placed.kicad_pcb (footprints, outline, keepouts, no copper) and
the project net classes and custom DRC rules next to it and in kicad/. Copper
is added by route_critical.py, KRT and finish_pcb.py (see make.sh).
"""
import json
import math
import sys
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design  # noqa: E402
import impedance  # noqa: E402
from placement import INTERFACE, placement  # noqa: E402
from sexpr import child, children, parse  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
KICAD = ROOT / 'kicad'
BUILD = ROOT / 'build'
PROJECT = 'voicestick-c3-v1'
STOCK_FP = Path('/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints')
STOCK_3D = Path('/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels')
RELEASE_MODEL_FALLBACKS = {
    'USB_C_Receptacle_HRO_TYPE-C-31-M-12.step',
    'LED_WS2812B-2020_PLCC4_2.0x2.0mm.step',
}
COPPER = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]

# JLC04101H-3313, 1.0 mm finished thickness.
STACKUP = '''
		(stackup
			(layer "F.SilkS" (type "Top Silk Screen"))
			(layer "F.Paste" (type "Top Solder Paste"))
			(layer "F.Mask" (type "Top Solder Mask") (thickness 0.01))
			(layer "F.Cu" (type "copper") (thickness 0.035))
			(layer "dielectric 1" (type "prepreg") (thickness 0.0994) (material "3313") (epsilon_r 4.1) (loss_tangent 0.02))
			(layer "In1.Cu" (type "copper") (thickness 0.0152))
			(layer "dielectric 2" (type "core") (thickness 0.69) (material "FR4") (epsilon_r 4.6) (loss_tangent 0.02))
			(layer "In2.Cu" (type "copper") (thickness 0.0152))
			(layer "dielectric 3" (type "prepreg") (thickness 0.0994) (material "3313") (epsilon_r 4.1) (loss_tangent 0.02))
			(layer "B.Cu" (type "copper") (thickness 0.035))
			(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.01))
			(layer "B.Paste" (type "Bottom Solder Paste"))
			(layer "B.SilkS" (type "Bottom Silk Screen"))
			(copper_finish "ENIG")
			(dielectric_constraints no)
		)'''

SIGNAL = dict(track=0.15, clearance=0.127, via=(0.45, 0.3))
POWER_VIA = (0.6, 0.3)


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    """Mechanical XY (mm) to a KiCad point."""
    return pcbnew.VECTOR2I(mm(100.0 + x), mm(100.0 - y))


# ------------------------------------------------------------------ netlist
def read_netlist():
    root = parse((KICAD / f'{PROJECT}.net').read_text())
    comps = {}
    for comp in children(child(root, 'components'), 'comp'):
        ref = str(child(comp, 'ref')[1])
        fields = {}
        for field in children(child(comp, 'fields'), 'field'):
            name = str(child(field, 'name')[1])
            fields[name] = str(field[2]) if len(field) > 2 else ''
        comps[ref] = dict(value=str(child(comp, 'value')[1]),
                          footprint=str(child(comp, 'footprint')[1]),
                          uuid=str(child(comp, 'tstamps')[1]), fields=fields)
    nets = {}
    for net in children(child(root, 'nets'), 'net'):
        name = str(child(net, 'name')[1])
        for node in children(net, 'node'):
            nets[(str(child(node, 'ref')[1]), str(child(node, 'pin')[1]))] = name
    return comps, nets


# -------------------------------------------------------------------- shapes
def add_outline(board):
    w, h = INTERFACE['board']['outline_xy']
    r = INTERFACE['board']['corner_radius']
    x0, x1, y0, y1 = -w / 2, w / 2, -h / 2, h / 2

    def line(a, b):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
        s.SetStart(pt(*a))
        s.SetEnd(pt(*b))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.05))
        board.Add(s)

    def arc(cx, cy, a0):
        s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
        p = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (a0, a0 + 45, a0 + 90)]
        s.SetArcGeometry(pt(*p[0]), pt(*p[1]), pt(*p[2]))
        s.SetLayer(pcbnew.Edge_Cuts)
        s.SetWidth(mm(0.05))
        board.Add(s)

    line((x0 + r, y0), (x1 - r, y0))
    line((x1, y0 + r), (x1, y1 - r))
    line((x1 - r, y1), (x0 + r, y1))
    line((x0, y1 - r), (x0, y0 + r))
    arc(x1 - r, y0 + r, 270)
    arc(x1 - r, y1 - r, 0)
    arc(x0 + r, y1 - r, 90)
    arc(x0 + r, y0 + r, 180)


def polygon(points):
    poly = pcbnew.SHAPE_POLY_SET()
    poly.NewOutline()
    for x, y in points:
        poly.Append(mm(100.0 + x), mm(100.0 - y))
    return poly


def rect(x0, x1, y0, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def circle(cx, cy, r, n=32):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def layer_set(layers):
    ls = pcbnew.LSET()
    for layer in layers:
        ls.AddLayer(layer)
    return ls


def add_rule_area(board, name, points, layers, tracks=False, vias=False, pours=False, footprints=False):
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetZoneName(name)
    z.SetLayerSet(layer_set(layers))
    z.SetDoNotAllowTracks(not tracks)
    z.SetDoNotAllowVias(not vias)
    z.SetDoNotAllowZoneFills(not pours)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowFootprints(not footprints)
    z.Outline().Append(polygon(points))
    board.Add(z)
    return z


def add_text(board, text, x, y, layer=pcbnew.F_SilkS, size=0.8, angle=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetPosition(pt(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * 0.15))
    t.SetTextAngleDegrees(angle)
    if layer in (pcbnew.B_SilkS, pcbnew.B_Fab):
        t.SetMirrored(True)
    board.Add(t)


# ---------------------------------------------------------------- footprints
def load_footprint(fpid):
    lib, name = fpid.split(':')
    path = KICAD / 'VoiceStickC3V1.pretty' if lib == design.LIB else STOCK_FP / f'{lib}.pretty'
    fp = pcbnew.FootprintLoad(str(path), name)
    assert fp, fpid
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    use_vendored_models(fp)
    return fp


def use_vendored_models(fp):
    """Keep project-relative paths usable when the optional model pack is unpacked later."""
    models = fp.Models()  # iterating yields copies; write back by index
    for i in range(len(models)):
        model = models[i]
        stock = Path(model.m_Filename.replace('${KICAD10_3DMODEL_DIR}', str(STOCK_3D)))
        if stock.name in RELEASE_MODEL_FALLBACKS and not stock.exists():
            model.m_Filename = '${KIPRJMOD}/3dmodels/' + stock.name
            models[i] = model


def add_footprints(board, comps, pin_nets, net_items):
    places = placement()
    missing = sorted(set(comps) - set(places))
    assert not missing, f'no placement for {missing}'
    for ref, comp in sorted(comps.items()):
        p = design.PARTS[ref]
        fp = load_footprint(comp['footprint'])
        board.Add(fp)
        fp.SetReference(ref)
        fp.SetValue(comp['value'])
        fp.SetPath(pcbnew.KIID_PATH('/' + comp['uuid']))
        fp.SetSheetname('/')
        fp.SetSheetfile(f'{PROJECT}.kicad_sch')
        x, y, rot = places[ref]
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        ref_text = fp.Reference()
        ref_text.SetLayer(pcbnew.F_Fab)
        ref_text.SetTextSize(pcbnew.VECTOR2I(mm(0.5), mm(0.5)))
        ref_text.SetTextThickness(mm(0.08))
        if p['dnp']:
            fp.SetDNP(True)
            fp.SetExcludedFromPosFiles(True)
        if p['block'] in ('test', 'mech'):
            fp.SetExcludedFromBOM(True)
            fp.SetExcludedFromPosFiles(True)
        # Mirror the schematic fields so the DRC parity check sees identical footprints.
        for name, value in comp['fields'].items():
            if name != 'Footprint':
                fp.SetField(name, value)
                field = fp.GetField(name)
                field.SetVisible(False)
                field.SetLayer(pcbnew.F_Fab)
        for pad in fp.Pads():
            net = pin_nets.get((ref, pad.GetNumber()))
            if net:
                pad.SetNet(net_items[net])


# ------------------------------------------------------------- board rules
def net_classes():
    """Net class table shared by the project file and the routing scripts."""
    classes = {
        'Default': dict(SIGNAL, nets=[]),
        'GND': dict(track=0.25, clearance=0.15, via=SIGNAL['via'], nets=['GND']),
        'RF': dict(track=impedance.RF_WIDTH, clearance=impedance.RF_GAP, via=SIGNAL['via'],
                   nets=['LNA_IN', 'ANT_FEED', 'ANT']),
        'USB': dict(track=0.2, clearance=0.15, via=SIGNAL['via'], nets=['USB_DP', 'USB_DN']),
        'XTAL': dict(track=0.15, clearance=0.2, via=SIGNAL['via'], nets=['X40_P', 'XTAL_P', 'XTAL_N']),
    }
    for net, width in design.POWER_NETS.items():
        high = net in design.HIGH_CURRENT_NETS
        name = ('PWR_' if high else 'RAIL_') + f'{width:.2f}'.replace('.', 'p')
        via = POWER_VIA if high else SIGNAL['via']
        classes.setdefault(name, dict(track=width, clearance=0.15, via=via, nets=[]))['nets'].append(net)
    return classes


def write_project(classes, stem):
    out_classes, patterns = [], []
    for priority, (name, c) in enumerate(classes.items()):
        out_classes.append({
            'name': name, 'clearance': c['clearance'], 'track_width': c['track'],
            'via_diameter': c['via'][0], 'via_drill': c['via'][1],
            'microvia_diameter': 0.3, 'microvia_drill': 0.1,
            'diff_pair_width': 0.2, 'diff_pair_gap': 0.15, 'diff_pair_via_gap': 0.25,
            'wire_width': 6, 'bus_width': 12, 'line_style': 0,
            'pcb_color': 'rgba(0, 0, 0, 0.000)', 'schematic_color': 'rgba(0, 0, 0, 0.000)',
            'priority': 2147483647 if name == 'Default' else priority,
        })
        patterns += [{'netclass': name, 'pattern': n} for n in c['nets']]
    rules = {
        'min_clearance': 0.1, 'min_track_width': 0.1, 'min_connection': 0.1,
        'min_via_diameter': 0.45, 'min_via_annular_width': 0.075,
        'min_through_hole_diameter': 0.3, 'min_hole_to_hole': 0.25, 'min_hole_clearance': 0.2,
        'min_copper_edge_clearance': 0.3, 'min_microvia_diameter': 0.3, 'min_microvia_drill': 0.1,
        'min_silk_clearance': 0.0, 'min_text_height': 0.6, 'min_text_thickness': 0.08,
        'solder_mask_to_copper_clearance': 0.0, 'max_error': 0.005,
        'allow_blind_buried_vias': False, 'allow_microvias': False, 'use_height_for_length_calcs': True,
    }
    severities = {
        # Reference designators live on F.Fab; silk is only board text.
        'silk_over_copper': 'warning', 'silk_overlap': 'warning', 'silk_edge_clearance': 'warning',
        'text_height': 'warning', 'text_thickness': 'warning',
        'lib_footprint_issues': 'ignore', 'lib_footprint_mismatch': 'ignore',
        'footprint_symbol_mismatch': 'error', 'courtyards_overlap': 'error',
    }
    project = {
        'meta': {'filename': f'{PROJECT}.kicad_pro', 'version': 3},
        'board': {'design_settings': {
            'defaults': {'board_outline_line_width': 0.05, 'copper_line_width': 0.2, 'copper_text_size_h': 1.0,
                         'copper_text_size_v': 1.0, 'copper_text_thickness': 0.15, 'silk_line_width': 0.12,
                         'silk_text_size_h': 0.8, 'silk_text_size_v': 0.8, 'silk_text_thickness': 0.12},
            'rules': rules,
            'rule_severities': severities,
            'track_widths': [0.0, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5],
            'via_dimensions': [{'diameter': 0.0, 'drill': 0.0}, {'diameter': 0.45, 'drill': 0.3},
                               {'diameter': 0.6, 'drill': 0.3}],
            'diff_pair_dimensions': [{'gap': 0.0, 'via_gap': 0.0, 'width': 0.0}],
        }},
        'net_settings': {'classes': out_classes, 'meta': {'version': 4}, 'net_colors': None,
                         'netclass_assignments': None, 'netclass_patterns': patterns},
        'pcbnew': {'page_layout_descr_file': ''},
        'schematic': {'drawing': {}, 'meta': {'version': 1}},
        'sheets': [],
        'text_variables': {},
    }
    project['meta']['filename'] = stem.name + '.kicad_pro'
    stem.with_suffix('.kicad_pro').write_text(json.dumps(project, indent=2) + '\n')


DRC_RULES = '''(version 1)

# JLC standard 4-layer vias with no surcharge; nothing smaller is allowed.
(rule "via size"
	(condition "A.Type == 'Via'")
	(constraint via_diameter (min 0.45mm))
	(constraint hole_size (min 0.3mm)))

# High-current nets only (design.HIGH_CURRENT_NETS, net classes PWR_*).
(rule "power via size"
	(condition "A.Type == 'Via' && A.NetClass == 'PWR_*'")
	(constraint via_diameter (min 0.6mm))
	(constraint hole_size (min 0.3mm)))

# The 50 ohm feed keeps its coplanar gap against every pour.
(rule "rf gap to pour"
	(condition "A.NetClass == 'RF' && B.Type == 'Zone'")
	(constraint clearance (min {gap}mm)))

# QFN pads at 0.5 mm pitch: allow the neck-down clearance at the pads only.
(rule "qfn fanout"
	(condition "A.Type == 'Pad' && A.Parent == 'U1'")
	(constraint clearance (min 0.1mm)))
'''


def write_rules(stem):
    stem.with_suffix('.kicad_dru').write_text(DRC_RULES.replace('{gap}', f'{impedance.RF_GAP:g}'))


# ------------------------------------------------------------------ keepouts
def add_keepouts(board):
    i = INTERFACE
    ant = i['antenna']
    ax0, ax1 = ant['copper_keepout_x']
    ay0, ay1 = ant['copper_keepout_y']
    area = rect(ax0, ax1 + 1.0, ay0, ay1 + 1.0)
    # The feed track enters on F.Cu; inner layers and the bottom stay empty.
    add_rule_area(board, 'antenna keepout top', area, [pcbnew.F_Cu], tracks=True, footprints=True)
    add_rule_area(board, 'antenna keepout inner+bottom', area, COPPER[1:], footprints=True)
    r = i['fasteners']['pcb_keepout_diameter'] / 2
    for hole in i['fasteners']['holes']:
        add_rule_area(board, f"{hole['ref']} screw keepout", circle(*hole['xy'], r), COPPER,
                      pours=True, footprints=True)
    # No via or track may leak air into the sealed microphone port on the bottom.
    add_rule_area(board, 'mic port seal', circle(*i['microphone']['port_xy'], 1.5), [pcbnew.B_Cu],
                  pours=True, footprints=True)
    # No bottom-layer track under the 40 MHz crystal (Espressif layout guide).
    bb = board.FindFootprintByReference('Y1').GetBoundingBox(False)
    x0, x1 = pcbnew.ToMM(bb.GetLeft()) - 100.0, pcbnew.ToMM(bb.GetRight()) - 100.0
    y0, y1 = 100.0 - pcbnew.ToMM(bb.GetBottom()), 100.0 - pcbnew.ToMM(bb.GetTop())
    add_rule_area(board, 'crystal keepout bottom', rect(x0, x1, y0, y1), [pcbnew.B_Cu],
                  vias=True, pours=True, footprints=True)


def add_board_text(board):
    # The top side is full, so the name and key labels go on the bottom silk.
    add_text(board, 'VoiceStick C3 v1', 6.0, -13.6, layer=pcbnew.B_SilkS, size=0.8)
    for ref, label in (('SW2', 'PREV'), ('SW3', 'NEXT')):
        x = next(k['x'] for k in INTERFACE['side_keys']['keys'] if k['ref'] == ref)
        add_text(board, label, x, -12.0, layer=pcbnew.B_SilkS, size=0.7)
    add_text(board, 'CANCEL', -11.0, 12.0, layer=pcbnew.B_SilkS, size=0.7)
    add_text(board, 'BAT+', 2.6, -14.2, size=0.6)


def clip_silk_to_board(board, margin=0.2):
    """Drop footprint silk that runs past the board outline (the USB-C shell
    outline overhangs the edge on purpose)."""
    w, h = INTERFACE['board']['outline_xy']
    for fp in board.GetFootprints():
        for item in list(fp.GraphicalItems()):
            if item.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
                continue
            box = item.GetBoundingBox()
            x0, x1 = pcbnew.ToMM(box.GetLeft()) - 100.0, pcbnew.ToMM(box.GetRight()) - 100.0
            y0, y1 = 100.0 - pcbnew.ToMM(box.GetBottom()), 100.0 - pcbnew.ToMM(box.GetTop())
            if x0 < -w / 2 + margin or x1 > w / 2 - margin or y0 < -h / 2 + margin or y1 > h / 2 - margin:
                fp.Remove(item)


def patch_stackup(path):
    text = path.read_text()
    assert '(stackup' not in text
    marker = '\t(setup'
    idx = text.index(marker) + len(marker)
    path.write_text(text[:idx] + STACKUP + text[idx:])


def main():
    pcbnew.KIID.SeedGenerator(design.KIID_SEEDS['build_pcb'])
    comps, pin_nets = read_netlist()
    # NewBoard reads an existing project file next to the board; start clean.
    for stale in BUILD.glob('01_placed.*'):
        stale.unlink()
    out = BUILD / '01_placed.kicad_pcb'
    out.parent.mkdir(exist_ok=True)
    board = pcbnew.NewBoard(str(out))
    board.SetCopperLayerCount(4)
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(4)
    ds.SetBoardThickness(mm(INTERFACE['board']['thickness']))
    ds.m_HasStackup = True

    net_items = {}
    for name in sorted(set(pin_nets.values())):
        item = pcbnew.NETINFO_ITEM(board, name)
        board.Add(item)
        net_items[name] = item

    add_outline(board)
    add_footprints(board, comps, pin_nets, net_items)
    clip_silk_to_board(board)
    add_keepouts(board)
    add_board_text(board)

    pcbnew.SaveBoard(str(out), board)
    patch_stackup(out)
    # SaveBoard rewrites the project file, so the net classes go in afterwards.
    for stem in (out.with_suffix(''), KICAD / PROJECT):
        write_project(net_classes(), stem)
        write_rules(stem)
    print(f'pcb written: {out} ({len(comps)} footprints, {len(net_items)} nets)')


if __name__ == '__main__':
    main()
