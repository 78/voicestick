#!/usr/bin/env python3
"""VoiceStick C3 v1 FDM enclosure, generated from assembly-interface.json.

Run with FreeCAD's bundled python (FreeCAD 1.x):
    /Applications/FreeCAD.app/Contents/Resources/bin/python enclosure/build_enclosure.py

Inputs
    assembly-interface.json          mechanical contract shared with the PCB
    enclosure/pcb-envelopes.json    component boxes from tools/export_envelopes.py
    fab/voicestick-c3-v1.step        PCB model (optional, only added to the assembly)

Outputs (enclosure/out/)
    step/<part>.step                 one file per printed part, assembly coordinates
    stl/<part>.stl                   printed parts, already in print orientation
    voicestick-c3-v1-assembly.step   enclosure + PCB + references, rest state
    enclosure-report.json            volumes, print data, interference results

Coordinates follow the interface file: board centre in XY, Z = 0 on the table,
+X towards USB, +Y towards the cancel key. Every modelled hole is enlarged by
fdm.hole_oversize so the printed hole lands on the nominal size.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, '/Applications/FreeCAD.app/Contents/Resources/lib')
import FreeCAD  # noqa: E402
import Mesh  # noqa: E402
import MeshPart  # noqa: E402
import Part  # noqa: E402

V = FreeCAD.Vector
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OUT = HERE / 'out'

IF = json.loads((ROOT / 'assembly-interface.json').read_text())
ENV = json.loads((HERE / 'pcb-envelopes.json').read_text())

BOARD, ZS, CASE, FDM = IF['board'], IF['z_stack'], IF['case'], IF['fdm']
OVERSIZE = FDM['hole_oversize']
CASE_X, CASE_Y = CASE['outer_xy']
CAVITY_X = CASE_X - 2 * CASE['wall']
CAVITY_Y = CASE_Y - 2 * CASE['wall']
CAVITY_R = CASE['outer_corner_radius'] - CASE['wall']
PCB_BOTTOM, PCB_TOP = BOARD['bottom_z'], BOARD['top_z']
SPLIT = ZS['split_z']
LIGHT = IF['light_window']
ROOF_LEDS = [led for led in LIGHT['leds'] if 'tube_inner_xy' in led]
END_LEDS = [led for led in LIGHT['leds'] if 'end_window_wh' in led]

# Side-key travel used for the pressed check: pusher gap + SKRTLBE010 stroke (0.2 mm).
SIDE_KEY_PRESS = IF['side_keys']['pusher_gap'] + 0.2
# Volume below which two solids only touch (shared faces of seated parts).
TOUCH_VOLUME = 1e-3


# --------------------------------------------------------------------------
# Primitive helpers
# --------------------------------------------------------------------------
def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def cylinder(diameter, z0, z1, x=0.0, y=0.0):
    return Part.makeCylinder(diameter / 2, z1 - z0, V(x, y, z0))


def fuse(shapes):
    shapes = list(shapes)
    if len(shapes) == 1:
        return shapes[0]
    return shapes[0].fuse(shapes[1:]).removeSplitter()


def rounded_box(size_x, size_y, z0, z1, radius, cx=0.0, cy=0.0):
    """Vertical prism with a rounded-rectangle section centred on (cx, cy)."""
    r = min(radius, size_x / 2 - 1e-3, size_y / 2 - 1e-3)
    x0, x1 = cx - size_x / 2, cx + size_x / 2
    y0, y1 = cy - size_y / 2, cy + size_y / 2
    if r < 0.01:
        return box(x0, x1, y0, y1, z0, z1)
    parts = [box(x0, x1, y0 + r, y1 - r, z0, z1), box(x0 + r, x1 - r, y0, y1, z0, z1)]
    for x in (x0 + r, x1 - r):
        for y in (y0 + r, y1 - r):
            parts.append(cylinder(2 * r, z0, z1, x, y))
    return fuse(parts)


def rounded_prism_y(size_x, size_z, y0, y1, radius, cx, cz):
    """Rounded-rectangle prism running along Y (windows in the side walls)."""
    shape = rounded_box(size_x, size_z, 0, y1 - y0, radius)
    shape.rotate(V(0, 0, 0), V(1, 0, 0), 90)  # extrusion now runs along -Y
    shape.translate(V(cx, y1, cz))
    return shape


def rounded_prism_x(size_y, size_z, x0, x1, radius, cy, cz):
    """Rounded-rectangle prism running along X (openings in the end walls)."""
    shape = rounded_box(size_z, size_y, 0, x1 - x0, radius)
    shape.rotate(V(0, 0, 0), V(0, 1, 0), 90)  # extrusion now runs along +X
    shape.translate(V(x0, cy, cz))
    return shape


def chamfer_face_edges(shape, z, size):
    """Chamfer the outer edges lying in the plane Z = z (first-layer elephant foot)."""
    edges = [e for e in shape.Edges
             if abs(e.BoundBox.ZMin - z) < 1e-6 and abs(e.BoundBox.ZMax - z) < 1e-6]
    outer = max(edges, key=lambda e: e.BoundBox.DiagonalLength)
    outer_box = outer.BoundBox
    ring = [e for e in edges if e.BoundBox.XMin < outer_box.XMin + 1e-3 + 2 * CASE['outer_corner_radius']
            or e.BoundBox.XMax > outer_box.XMax - 1e-3 - 2 * CASE['outer_corner_radius']
            or e.BoundBox.YMin < -CASE_Y / 2 + 0.01 or e.BoundBox.YMax > CASE_Y / 2 - 0.01]
    ring = [e for e in ring if _on_outer_profile(e)]
    try:
        return shape.makeChamfer(size, ring)
    except Part.OCCError:
        print('  warning: first-layer chamfer failed, left sharp')
        return shape


def _on_outer_profile(edge):
    """True when the edge lies on the case outline (offset 0)."""
    p = edge.valueAt((edge.FirstParameter + edge.LastParameter) / 2)
    r = CASE['outer_corner_radius']
    dx = max(abs(p.x) - (CASE_X / 2 - r), 0.0)
    dy = max(abs(p.y) - (CASE_Y / 2 - r), 0.0)
    return abs((dx * dx + dy * dy) ** 0.5 - r) < 1e-3


def hole(nominal, z0, z1, x, y):
    """Printed hole: modelled with the FDM oversize."""
    return cylinder(nominal + OVERSIZE, z0, z1, x, y)


# --------------------------------------------------------------------------
# Shared features
# --------------------------------------------------------------------------
def mic_duct_cut():
    """Floor groove from the port to the -X end face; it becomes a tunnel in the wall."""
    duct = IF['microphone']['duct']
    port_y = IF['microphone']['port_xy'][1]
    groove_top = ZS['floor_top_z']
    half = duct['groove_width'] / 2
    return box(-CASE_X / 2 - 0.1, duct['groove_from_x'], port_y - half, port_y + half,
               groove_top - duct['groove_depth'], groove_top + 0.01)


def side_key_geometry(key):
    """Window, inner pocket and cap dimensions of one side key, wall-relative."""
    sk = IF['side_keys']
    side = key['side']
    face_x, face_z = sk['cap_face_xy']
    clearance = sk['cap_clearance']
    flange_x, flange_z = face_x + 2 * sk['cap_flange_extra'], face_z + 2 * sk['cap_flange_extra']
    return {
        'side': side,
        'x': key['x'],
        'z': PCB_TOP + sk['actuation_above_pcb'],
        'face': (face_x, face_z),
        'window': (face_x + 2 * clearance, face_z + 2 * clearance),
        'flange': (flange_x, flange_z),
        'pocket': (flange_x + 2 * clearance, flange_z + 2 * clearance),
        'wall_inner': CAVITY_Y / 2,
        'wall_outer': CASE_Y / 2,
        'pocket_depth': CASE['wall'] - FDM['min_feature'],
        'flange_inner': sk['plunger_tip_abs_y'] + sk['pusher_gap'],
    }


def y_span(side, a, b):
    """Return (y0, y1) for the span |y| in [a, b] on the given side."""
    return (a, b) if side > 0 else (-b, -a)


def floor_relief_holes(z0, z1):
    """Clearance for through-hole leads that stick out under the PCB."""
    cuts = []
    for lead in ENV['tht']:
        dx, dy = lead['drill']
        if lead['angle'] % 180 == 90:
            dx, dy = dy, dx
        clearance = 0.6
        if abs(dx - dy) < 1e-6:
            cuts.append(cylinder(dx + clearance, z0, z1, *lead['xy']))
        else:
            cuts.append(rounded_box(dx + clearance, dy + clearance, z0, z1,
                                    min(dx, dy) / 2 + clearance / 2, *lead['xy']))
    return cuts


# --------------------------------------------------------------------------
# Lower shell
# --------------------------------------------------------------------------
def build_lower_shell(sealed_duct=True):
    """Lower shell. sealed_duct=False gives the A/B comparison shell: no groove,
    three 0.6 mm holes in the end face open into the case cavity instead."""
    z_bottom, floor_top = ZS['shell_bottom_z'], ZS['floor_top_z']
    r = CASE['outer_corner_radius']
    shell = rounded_box(CASE_X, CASE_Y, z_bottom, SPLIT, r)
    shell = chamfer_face_edges(shell, z_bottom, FDM['first_layer_chamfer'])
    shell = shell.cut(rounded_box(CAVITY_X, CAVITY_Y, floor_top, SPLIT + 1, CAVITY_R))

    # Tongue that locates the upper shell: a ring between offsets 1.0 and 1.8 mm.
    tongue_out, tongue_in = 1.0, 1.0 + FDM['lip_width']
    tongue = rounded_box(CASE_X - 2 * tongue_out, CASE_Y - 2 * tongue_out,
                         SPLIT - 0.5, SPLIT + FDM['lip_height'], r - tongue_out)
    tongue = tongue.cut(rounded_box(CASE_X - 2 * tongue_in, CASE_Y - 2 * tongue_in,
                                    SPLIT - 1, SPLIT + FDM['lip_height'] + 1, r - tongue_in))
    additions = [tongue]

    fasteners = IF['fasteners']
    for h in fasteners['holes']:
        additions.append(cylinder(fasteners['lower_boss_outer_diameter'], floor_top - 0.1,
                                  ZS['pcb_support_top_z'], *h['xy']))
    for s in IF['pcb_supports']:
        additions.append(cylinder(s['diameter'], floor_top - 0.1, ZS['pcb_support_top_z'], *s['xy']))
    shell = fuse([shell] + additions)

    if sealed_duct:
        cuts = [mic_duct_cut()]
    else:
        port_y = IF['microphone']['port_xy'][1]
        cuts = [rounded_prism_x(0.6 + OVERSIZE, 0.6 + OVERSIZE, -CASE_X / 2 - 0.1, -CAVITY_X / 2 + 0.1,
                                0.39, port_y + dy, PCB_TOP + 2.5) for dy in (-1.5, 0.0, 1.5)]
    cb_d, cb_depth = fasteners['head_counterbore']
    for h in fasteners['holes']:
        cuts.append(hole(fasteners['lower_clearance_hole'], z_bottom - 0.1, PCB_BOTTOM + 0.1, *h['xy']))
        cuts.append(hole(cb_d, z_bottom - 0.1, z_bottom + cb_depth, *h['xy']))
    cuts += floor_relief_holes(z_bottom - 0.1, floor_top + 0.1)

    # USB opening, with the wall locally thinned from the inside to usb_end_wall.
    usb = IF['usb']
    open_y, open_z = usb['opening_wz']
    open_cz = PCB_TOP + (open_z - 2 * 0.4) / 2
    cuts.append(rounded_prism_x(open_y, open_z, CASE_X / 2 - 2.0, CASE_X / 2 + 0.1,
                                usb['opening_corner_radius'], 0.0, open_cz))
    cuts.append(box(CAVITY_X / 2 - 0.1, CASE_X / 2 - CASE['usb_end_wall'],
                    -open_y / 2 - 1.2, open_y / 2 + 1.2, PCB_BOTTOM, SPLIT - 0.5))
    # Side-looking LEDs: the end wall thinned from the inside in front of the lens.
    for led in END_LEDS:
        wy, wz = led['end_window_wh']
        ly, lz = led['xy'][1], PCB_TOP + led['lens_above_pcb']
        cuts.append(rounded_prism_x(wy, wz, CAVITY_X / 2 - 0.1, CASE_X / 2 - LIGHT['window_wall'],
                                    0.3, ly, lz))

    for key in IF['side_keys']['keys']:
        g = side_key_geometry(key)
        wx, wz = g['window']
        y0, y1 = y_span(g['side'], g['wall_inner'] - 0.5, g['wall_outer'] + 0.1)
        cuts.append(rounded_prism_y(wx, wz, y0, y1, 0.5, g['x'], g['z']))
        px, pz = g['pocket']
        y0, y1 = y_span(g['side'], g['wall_inner'] - 0.1, g['wall_inner'] + g['pocket_depth'])
        cuts.append(rounded_prism_y(px, pz, y0, y1, 0.6, g['x'], g['z']))
    return shell.cut(fuse(cuts))


# --------------------------------------------------------------------------
# Upper shell
# --------------------------------------------------------------------------
def build_upper_shell(component_keepouts):
    top, roof_inner = ZS['body_top_z'], ZS['roof_inner_z']
    r = CASE['outer_corner_radius']
    skin = FDM['lip_width']
    relief_top = SPLIT + FDM['lip_height'] + FDM['lip_clearance']

    shell = rounded_box(CASE_X, CASE_Y, SPLIT, top, r)
    shell = chamfer_face_edges(shell, top, FDM['first_layer_chamfer'])
    shell = shell.cut(rounded_box(CAVITY_X, CAVITY_Y, SPLIT - 1, roof_inner, CAVITY_R))
    shell = shell.cut(rounded_box(CASE_X - 2 * skin, CASE_Y - 2 * skin, SPLIT - 1, relief_top, r - skin))

    additions = []
    fasteners = IF['fasteners']
    for h in fasteners['holes']:
        additions.append(cylinder(fasteners['upper_post_outer_diameter'], PCB_TOP, roof_inner + 0.1, *h['xy']))

    # One light tube per roof LED, from the roof down to just above the board.
    wall = LIGHT['tube_wall']
    tube_bottom = PCB_TOP + LIGHT['tube_bottom_gap']
    for led in ROOF_LEDS:
        tx, ty = led['xy']
        ix, iy = led['tube_inner_xy']
        tube = rounded_box(ix + 2 * wall, iy + 2 * wall, tube_bottom, roof_inner + 0.1, 0.4, tx, ty)
        tube = tube.cut(box(tx - ix / 2, tx + ix / 2, ty - iy / 2, ty + iy / 2, tube_bottom - 1, roof_inner))
        additions.append(tube.cut(component_keepouts))

    # Battery locating rib on the USB end of the pack; the long sides are the walls
    # and the main-key end is the key opening, so the pack is taped to the roof.
    bat = IF['battery']
    bx, by = bat['center_xy']
    rib_x0 = bx + bat['envelope_xyz'][0] / 2 + FDM['locate_clearance'] * 2
    rib_height = 1.5
    additions.append(box(rib_x0, rib_x0 + FDM['min_feature'], -CAVITY_Y / 2, CAVITY_Y / 2,
                         roof_inner - rib_height, roof_inner + 0.1))
    shell = fuse([shell] + additions)

    cuts = []
    for h in fasteners['holes']:
        cuts.append(hole(fasteners['upper_pilot_hole'], PCB_TOP - 0.1, roof_inner - 1.2, *h['xy']))
    mk = IF['main_key']
    ox, oy = mk['cap_roof_opening_xy']
    cuts.append(rounded_box(ox, oy, roof_inner - 2, top + 1, 2.0, *mk['center_xy']))
    for led in ROOF_LEDS:
        tx, ty = led['xy']
        ix, iy = led['tube_inner_xy']
        cuts.append(box(tx - ix / 2, tx + ix / 2, ty - iy / 2, ty + iy / 2,
                        roof_inner - 0.1, top - LIGHT['window_wall']))
    return shell.cut(fuse(cuts))


# --------------------------------------------------------------------------
# Key caps
# --------------------------------------------------------------------------
def cross_socket(cx, cy, z0, depth, fit):
    sock = IF['main_key']['stem_socket']
    long_, width = sock['cross_long'] + fit, sock['cross_width'] + fit
    return fuse([box(cx - long_ / 2, cx + long_ / 2, cy - width / 2, cy + width / 2, z0, z0 + depth),
                 box(cx - width / 2, cx + width / 2, cy - long_ / 2, cy + long_ / 2, z0, z0 + depth)])


def main_cap_plate_bottom():
    mk = IF['main_key']
    return PCB_TOP + mk['housing_top_above_pcb'] + mk['stem_above_housing']


def build_main_cap(fit=0.0):
    mk = IF['main_key']
    cx, cy = mk['center_xy']
    sx, sy = mk['cap_xy']
    wall, plate = mk['cap_wall'], mk['cap_top_thickness']
    depth = mk['stem_socket']['socket_depth']
    plate_bottom = main_cap_plate_bottom()
    skirt = 2.0

    cap = rounded_box(sx, sy, plate_bottom - skirt, plate_bottom + plate, 1.5, cx, cy)
    cap = cap.cut(rounded_box(sx - 2 * wall, sy - 2 * wall, plate_bottom - skirt - 1, plate_bottom, 0.5, cx, cy))
    boss = cylinder(5.5, plate_bottom - depth, plate_bottom + 0.1, cx, cy)
    cap = fuse([cap, boss])
    return cap.cut(cross_socket(cx, cy, plate_bottom - depth - 0.1, depth + 0.1, fit))


def build_side_cap(key):
    g = side_key_geometry(key)
    side = g['side']
    flange_outer = g['wall_inner'] + g['pocket_depth']
    fx, fz = g['flange']
    y0, y1 = y_span(side, g['flange_inner'], flange_outer)
    flange = rounded_prism_y(fx, fz, y0, y1, 0.5, g['x'], g['z'])
    face_x, face_z = g['face']
    protrusion = 0.6
    y0, y1 = y_span(side, flange_outer - 0.01, g['wall_outer'] + protrusion)
    face = rounded_prism_y(face_x, face_z, y0, y1, 0.5, g['x'], g['z'])
    cap = fuse([flange, face])

    # The flange hangs below the PCB top; bevel its inner lower corner at 45 deg so a
    # pressed cap clears the board edge. The bevel grows away from the bed: no support.
    clear_z = PCB_TOP + 0.1
    pressed_edge = BOARD['outline_xy'][1] / 2 + SIDE_KEY_PRESS + 0.1
    bottom = g['z'] - fz / 2 - 0.1
    profile = [(g['flange_inner'] - 0.1, bottom), (g['flange_inner'] - 0.1, clear_z),
               (pressed_edge, clear_z), (pressed_edge + clear_z - bottom, bottom)]
    x0 = g['x'] - fx / 2 - 0.1
    points = [V(x0, side * y, z) for y, z in profile]
    bevel = Part.Face(Part.makePolygon(points + points[:1])).extrude(V(fx + 0.2, 0, 0))
    return cap.cut(bevel).removeSplitter()


# --------------------------------------------------------------------------
# Test prints
# --------------------------------------------------------------------------
def build_tolerance_coupon():
    """40 x 20 mm plate: socket fits, pilot holes, side-key windows, USB opening, duct tunnel.

    Left to right: cross sockets -0.05 / 0 / +0.05, pilot holes 1.5 / 1.6 / 1.7 (nominal,
    modelled with the oversize), duct groove with a 1.6 mm tunnel. The back wall holds the
    side-key windows with 0.2 / 0.25 / 0.3 clearance and the USB-C opening.
    """
    base_t = ZS['floor_thickness']
    coupon = rounded_box(40, 20, 0, base_t, 1.0, 20, 10)
    parts = [coupon]
    mk = IF['main_key']['stem_socket']
    boss_h = base_t + mk['socket_depth'] + 0.6
    socket_x = (4.0, 10.5, 17.0)
    for x in socket_x:
        parts.append(cylinder(5.5, 0, boss_h, x, 4.5))
    pilot_x = (23.5, 29.5, 35.5)
    for x in pilot_x:
        parts.append(cylinder(IF['fasteners']['upper_post_outer_diameter'], 0, 7.0, x, 4.5))
    wall_y0, wall_h = 18.4, 8.0
    parts.append(box(0.5, 39.5, wall_y0, 20, 0, wall_h))
    parts.append(box(14.4, 16.0, 9.0, 15.0, 0, 3.0))  # tunnel bridge over the groove
    coupon = fuse(parts)

    cuts = []
    for x, fit in zip(socket_x, mk['fit_offsets']):
        cuts.append(cross_socket(x, 4.5, boss_h - mk['socket_depth'], mk['socket_depth'] + 0.1, fit))
    for x, d in zip(pilot_x, IF['fasteners']['pilot_coupon']):
        cuts.append(hole(d, base_t, 7.1, x, 4.5))
    duct = IF['microphone']['duct']
    cuts.append(box(2.0, 17.0, 12.0 - duct['groove_width'] / 2, 12.0 + duct['groove_width'] / 2,
                    base_t - duct['groove_depth'], base_t + 0.01))
    face_x, face_z = IF['side_keys']['cap_face_xy']
    for x, c in zip((5.0, 12.5, 20.0), (0.2, 0.25, 0.3)):
        cuts.append(rounded_prism_y(face_x + 2 * c, face_z + 2 * c, wall_y0 - 0.1, 20.1, 0.5, x, 4.0))
    usb = IF['usb']
    open_y, open_z = usb['opening_wz']
    cuts.append(rounded_prism_y(open_y, open_z, wall_y0 - 0.1, 20.1, usb['opening_corner_radius'],
                                31.5, base_t + 0.6 + open_z / 2))
    return coupon.cut(fuse(cuts))


def build_acoustic_test_box(lower_shell):
    """Main-key end of the lower shell: duct, foam seat, H2 boss, corner support, end wall.

    Screw the real PCB on at H2 with an M2 screw and nut (no upper shell) and compare the
    microphone with and without the foam to confirm the duct is sealed.
    """
    clip = box(-CASE_X / 2 - 1, -14.5, -CASE_Y / 2 - 1, CASE_Y / 2 + 1, 0, SPLIT + 2)
    return lower_shell.common(clip).removeSplitter()


# --------------------------------------------------------------------------
# Reference bodies (not printed): PCB, components, switch, battery, foam, feet
# --------------------------------------------------------------------------
def build_pcb():
    bx, by = BOARD['outline_xy']
    pcb = rounded_box(bx, by, PCB_BOTTOM, PCB_TOP, BOARD['corner_radius'])
    holes = [cylinder(IF['fasteners']['pcb_hole_diameter'], PCB_BOTTOM - 0.1, PCB_TOP + 0.1, *h['xy'])
             for h in IF['fasteners']['holes']]
    return pcb.cut(fuse(holes))


def build_usb_receptacle(box_xy):
    usb = IF['usb']
    x0 = box_xy[0]
    body_y0, body_y1 = usb['body_y']
    h = usb['height_above_pcb']
    shell = rounded_prism_x(body_y1 - body_y0, h, x0, usb['front_x'], 1.2, 0.0, PCB_TOP + h / 2)
    pads = box(x0, BOARD['outline_xy'][0] / 2, box_xy[2], box_xy[3], PCB_TOP, PCB_TOP + 0.3)
    return fuse([shell, pads])


def build_components():
    """One solid per part on the PCB top, keyed by reference."""
    bodies = {}
    for c in ENV['components']:
        x0, x1, y0, y1 = c['box']
        if c['ref'] == IF['usb']['ref']:
            bodies[c['ref']] = build_usb_receptacle(c['box'])
        else:
            bodies[c['ref']] = box(x0, x1, y0, y1, PCB_TOP, PCB_TOP + c['height'])
    for lead in ENV['tht']:
        if lead['ref'] == IF['main_key']['ref']:
            continue
        d = min(lead['drill']) - 0.1
        bodies.setdefault(f"{lead['ref']}_leads", []).append(
            cylinder(d, PCB_BOTTOM - lead['below_pcb'], PCB_BOTTOM, *lead['xy']))
    for key in [k for k in bodies if k.endswith('_leads')]:
        bodies[key] = fuse(bodies[key])
    return bodies


def build_main_switch():
    """Static housing (with the slider window) and the moving stem of SW1."""
    mk = IF['main_key']
    cx, cy = mk['center_xy']
    bx, by = mk['body_xy']
    housing_top = PCB_TOP + mk['housing_top_above_pcb']
    lower = box(cx - bx / 2, cx + bx / 2, cy - by / 2, cy + by / 2, PCB_TOP, PCB_TOP + 5.0)
    upper = box(cx - 7.0, cx + 7.0, cy - 7.0, cy + 7.0, PCB_TOP + 5.0, housing_top)
    window = box(cx - 3.6, cx + 3.6, cy - 2.9, cy + 2.9, housing_top - mk['travel'] - 0.5, housing_top + 1)
    tail = []
    for lead in ENV['tht']:
        if lead['ref'] == mk['ref']:
            d = min(lead['drill']) - 0.2
            tail.append(cylinder(d, PCB_TOP - mk['tail_below_pcb_top'], PCB_BOTTOM, *lead['xy']))
    housing = fuse([lower, upper] + tail).cut(window)
    stem_top = housing_top + mk['stem_above_housing']
    stem = fuse([box(cx - 2.0, cx + 2.0, cy - 0.585, cy + 0.585, housing_top, stem_top),
                 box(cx - 0.585, cx + 0.585, cy - 2.0, cy + 2.0, housing_top, stem_top)])
    return housing, stem


def build_battery():
    bat = IF['battery']
    ex, ey, ez = bat['envelope_xyz']
    cx, cy = bat['center_xy']
    return rounded_box(ex, ey, bat['bottom_z'], bat['bottom_z'] + ez, 1.0, cx, cy)


def build_measured_battery():
    """The pack as measured, taped to the roof (the envelope adds swelling margin)."""
    bat = IF['battery']
    mx, my, mz = bat['measured_xyz']
    cx, cy = bat['center_xy']
    return rounded_box(mx, my, ZS['roof_inner_z'] - mz, ZS['roof_inner_z'], 1.0, cx, cy)


def build_foam():
    """Compressed foam gasket, clipped to the cavity, with the port hole."""
    foam = IF['microphone']['foam']
    x0 = max(foam['rect_x'][0], -CAVITY_X / 2)
    z0 = ZS['floor_top_z']
    pad = box(x0, foam['rect_x'][1], foam['rect_y'][0], foam['rect_y'][1], z0, z0 + foam['compressed_thickness'])
    port = cylinder(foam['hole_diameter'], z0 - 0.1, z0 + 1, *IF['microphone']['port_xy'])
    return pad.cut(port)


def build_feet():
    """Adhesive silicone bumpers, 6 mm, over the screw heads and the corner supports."""
    spots = ([h['xy'] for h in IF['fasteners']['holes']]
             + [s['xy'] for s in IF['pcb_supports'] if s.get('foot')])
    return fuse([cylinder(6.0, 0.0, ZS['foot_height'], *xy) for xy in spots]), spots


def component_keepouts(components, margin=0.2):
    """Inflated component boxes, cut from features that hang close to the PCB."""
    cuts = []
    for ref, body in components.items():
        if ref.endswith('_leads'):
            continue
        bb = body.BoundBox
        cuts.append(box(bb.XMin - margin, bb.XMax + margin, bb.YMin - margin, bb.YMax + margin,
                        PCB_TOP - 0.1, bb.ZMax + margin))
    return fuse(cuts)


# --------------------------------------------------------------------------
# Checks and export
# --------------------------------------------------------------------------
def moved(shape, dx=0.0, dy=0.0, dz=0.0):
    s = shape.copy()
    s.translate(V(dx, dy, dz))
    return s


def interference(bodies, ignore=()):
    """Return [(a, b, volume)] for every overlapping pair, skipping pairs in ignore."""
    names = sorted(bodies)
    clashes = []
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            if {a, b} in [set(p) for p in ignore]:
                continue
            sa, sb = bodies[a], bodies[b]
            if not sa.BoundBox.intersect(sb.BoundBox):
                continue
            volume = sa.common(sb).Volume
            if volume > TOUCH_VOLUME:
                clashes.append((a, b, round(volume, 4)))
    return clashes


def clearances(checks):
    """Minimum gap of each (label, shape_a, shape_b, required_mm)."""
    rows = []
    for label, a, b, required in checks:
        gap = a.distToShape(b)[0]
        rows.append({'check': label, 'gap_mm': round(gap, 3), 'required_mm': required,
                     'ok': gap >= required - 1e-6})
    return rows


def print_oriented(shape, flip_axis=None, angle=0.0):
    s = shape.copy()
    if flip_axis is not None:
        s.rotate(V(0, 0, 0), flip_axis, angle)
    bb = s.BoundBox
    s.translate(V(-(bb.XMin + bb.XMax) / 2, -(bb.YMin + bb.YMax) / 2, -bb.ZMin))
    return s


def export_stl(shape, path):
    mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=0.01, AngularDeflection=0.1, Relative=False)
    mesh.write(str(path))
    return {'facets': mesh.CountFacets, 'solid': bool(mesh.isSolid()),
            'non_manifold': bool(mesh.hasNonManifolds())}


def export_assembly(named_shapes, path):
    doc = FreeCAD.newDocument('VoiceStickC3V1')
    objects = []
    for name, shape in named_shapes.items():
        obj = doc.addObject('Part::Feature', name)
        obj.Shape = shape
        objects.append(obj)
    import Import
    Import.export(objects, str(path))
    FreeCAD.closeDocument(doc.Name)


def load_pcb_step():
    step = ROOT / 'fab' / 'voicestick-c3-v1.step'
    if not step.exists():
        return None
    shape = Part.read(str(step))
    # KiCad STEP: X = kicad_x, Y = -kicad_y, Z = 0 at the board bottom.
    shape.translate(V(-100.0, 100.0, PCB_BOTTOM))
    return shape


PRINT_SETTINGS = {
    'LowerShell': ('bottom face on the bed', '0.2 mm layers, 4 walls, 20 % gyroid, no supports'),
    'UpperShell': ('roof on the bed (flipped)', '0.2 mm layers, 4 walls, 20 % gyroid, no supports; '
                   'light-coloured filament for the LED window'),
    'MainCap': ('top face on the bed (flipped)', '0.12 mm layers, 100 % infill, no supports'),
    'SideCap_SW2': ('flange inner face on the bed', '0.12 mm layers, 100 % infill, no supports'),
    'SideCap_SW3': ('flange inner face on the bed', '0.12 mm layers, 100 % infill, no supports'),
    'SideCap_SW4': ('flange inner face on the bed', '0.12 mm layers, 100 % infill, no supports'),
    'ToleranceCoupon': ('as modelled', 'same settings as the shells; print first'),
    'AcousticTestBox': ('as modelled', 'same settings as the lower shell'),
    'LowerShell_OpenDuct': ('bottom face on the bed', 'A/B sound test only, same settings as the lower shell'),
}


def main():
    (OUT / 'step').mkdir(parents=True, exist_ok=True)
    (OUT / 'stl').mkdir(parents=True, exist_ok=True)

    print('building reference bodies')
    pcb = build_pcb()
    components = build_components()
    sw1_housing, sw1_stem = build_main_switch()
    battery, foam = build_battery(), build_foam()
    feet, foot_spots = build_feet()

    print('building printed parts')
    lower = build_lower_shell()
    upper = build_upper_shell(component_keepouts(components))
    main_cap = build_main_cap()
    side_caps = {f"SideCap_{k['ref']}": build_side_cap(k) for k in IF['side_keys']['keys']}
    printed = {'LowerShell': lower, 'UpperShell': upper, 'MainCap': main_cap, **side_caps}
    tests = {'ToleranceCoupon': build_tolerance_coupon(), 'AcousticTestBox': build_acoustic_test_box(lower),
             'LowerShell_OpenDuct': build_lower_shell(sealed_duct=False)}

    for name, shape in {**printed, **tests}.items():
        if not shape.isValid() or len(shape.Solids) != 1:
            raise SystemExit(f'{name}: invalid shape or {len(shape.Solids)} solids')

    references = {'PCB': pcb, 'SW1_housing': sw1_housing, 'SW1_stem': sw1_stem,
                  'Battery': battery, 'MicFoam': foam, 'Feet': feet}
    references.update({f'C_{ref}': body for ref, body in components.items()})

    print('checking interference')
    rest = {**printed, **references}
    # Seated pairs: the stem sits in the socket and the feet are glued to the shell.
    seated = [('MainCap', 'SW1_stem')]
    report = {'rest': interference(rest, seated)}

    travel = IF['main_key']['travel']
    pressed = dict(rest)
    pressed['MainCap'] = moved(main_cap, dz=-travel)
    pressed['SW1_stem'] = moved(sw1_stem, dz=-travel)
    report['main_key_pressed'] = interference(pressed, seated)

    for key in IF['side_keys']['keys']:
        name = f"SideCap_{key['ref']}"
        state = dict(rest)
        state[name] = moved(side_caps[name], dy=-key['side'] * SIDE_KEY_PRESS)
        # The cap is meant to push the switch plunger, which is inside the part box.
        report[f"{key['ref']}_pressed"] = interference(state, seated + [(name, f"C_{key['ref']}")])

    print('measuring clearances')
    parts_on_pcb = fuse([body for ref, body in components.items() if not ref.endswith('_leads')])
    everything_on_pcb = fuse(list(components.values()))
    pressed_cap = pressed['MainCap']
    checks = [
        ('battery envelope (6.5 mm, swelling margin) to PCB parts', battery, parts_on_pcb, 0.3),
        ('measured battery (5.8 mm) to PCB parts', build_measured_battery(), parts_on_pcb, 0.5),
        ('battery envelope to main switch', battery, sw1_housing, 0.5),
        ('upper shell (posts, light tube) to PCB parts', upper, parts_on_pcb, 0.2),
        ('lower shell to PCB parts and leads', lower, everything_on_pcb, 0.3),
        ('USB receptacle to case', fuse([lower, upper]), components[IF['usb']['ref']], 0.3),
        ('main cap pressed to switch housing (plate stop)', pressed_cap, sw1_housing, 0.1),
        ('main cap pressed to upper shell', pressed_cap, upper, 0.2),
    ]
    for key in IF['side_keys']['keys']:
        cap = side_caps[f"SideCap_{key['ref']}"]
        pushed = moved(cap, dy=-key['side'] * SIDE_KEY_PRESS)
        checks.append((f"{key['ref']} cap at rest to switch", cap, components[key['ref']], 0.1))
        checks.append((f"{key['ref']} cap pressed to PCB", pushed, pcb, 0.1))
    clearance_rows = clearances(checks)

    flips = {'UpperShell': (V(1, 0, 0), 180), 'MainCap': (V(1, 0, 0), 180)}
    for key in IF['side_keys']['keys']:
        flips[f"SideCap_{key['ref']}"] = (V(1, 0, 0), 90 if key['side'] > 0 else -90)

    print('exporting')
    parts_report = {}
    for name, shape in {**printed, **tests}.items():
        shape.exportStep(str(OUT / 'step' / f'{name}.step'))
        axis, angle = flips.get(name, (None, 0.0))
        oriented = print_oriented(shape, axis, angle)
        mesh = export_stl(oriented, OUT / 'stl' / f'{name}.stl')
        bb = oriented.BoundBox
        orientation, settings = PRINT_SETTINGS[name]
        parts_report[name] = {'volume_mm3': round(shape.Volume, 1),
                              'print_size_mm': [round(bb.XLength, 2), round(bb.YLength, 2), round(bb.ZLength, 2)],
                              'orientation': orientation, 'settings': settings, 'stl': mesh}

    assembly = {**printed, 'Battery': battery, 'MicFoam': foam, 'Feet': feet}
    pcb_step = load_pcb_step()
    if pcb_step is not None:
        # The board STEP carries a model for every part, SW1 included.
        assembly['PCB_assembly'] = pcb_step
    else:
        assembly.update({'PCB': pcb, 'SW1_housing': sw1_housing, 'SW1_stem': sw1_stem})
    export_assembly(assembly, OUT / 'voicestick-c3-v1-assembly.step')

    clashes = {state: [{'a': a, 'b': b, 'volume_mm3': v} for a, b, v in pairs] for state, pairs in report.items()}
    summary = {
        'source': 'assembly-interface.json rev ' + IF['revision'],
        'parts': parts_report,
        'feet_xy': foot_spots,
        'interference': clashes,
        'interference_free': all(not pairs for pairs in report.values()),
        'clearances': clearance_rows,
        'pcb_step_in_assembly': pcb_step is not None,
    }
    (OUT / 'enclosure-report.json').write_text(json.dumps(summary, indent=1))

    for name, info in parts_report.items():
        s = info['stl']
        print(f"  {name:16s} {info['volume_mm3']:8.1f} mm3  {info['print_size_mm']}  "
              f"solid={s['solid']} non_manifold={s['non_manifold']}")
    for state, pairs in report.items():
        print(f"  interference {state:18s} {'none' if not pairs else pairs}")
    for row in clearance_rows:
        print(f"  {'ok ' if row['ok'] else 'LOW'} {row['gap_mm']:5.2f} >= {row['required_mm']:.2f}  {row['check']}")
    if not summary['interference_free']:
        raise SystemExit('interference found, see out/enclosure-report.json')
    if not all(row['ok'] for row in clearance_rows):
        raise SystemExit('clearance below the requirement, see out/enclosure-report.json')
    bad_mesh = [n for n, info in parts_report.items() if not info['stl']['solid'] or info['stl']['non_manifold']]
    if bad_mesh:
        raise SystemExit(f'bad STL mesh: {bad_mesh}')
    print('enclosure OK')


if __name__ == '__main__':
    main()
