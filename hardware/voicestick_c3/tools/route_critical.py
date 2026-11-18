#!/usr/bin/env python3
"""Hand-route the critical copper and lock it before KRT touches the board.

Run with KiCad's bundled python. Reads build/01_placed.kicad_pcb, writes
build/02_critical.kicad_pcb. Covered here:
  - 50 ohm antenna feed with the chip and antenna matches, their shunt ground
    vias and a via fence
  - 40 MHz crystal loop (pins 29/30, L2, Y1, C14/C15) and its ground vias
  - 32.768 kHz crystal and the VDD3P3 L/C filter on pins 2-5
  - USB D+/D- from the C3 through the ESD array to both rows of the receptacle,
    the VBUS pad pair and the ESD rail
  - +3V3 decoupling on pins 31/32, the U1 exposed-pad ground vias and the
    escape vias of pins 27/28 that sit between the USB pair and the crystal
Everything is placed from pad coordinates, so moving a footprint in
placement.py moves its copper with it.
"""
import shutil
import sys
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design  # noqa: E402
import impedance  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
SIGNAL_VIA = (0.45, 0.3)
POWER_VIA = (0.6, 0.3)


def mm(v):
    return pcbnew.FromMM(v)


def to_kicad(xy):
    return pcbnew.VECTOR2I(mm(100.0 + xy[0]), mm(100.0 - xy[1]))


class Copper:
    """Small helper that lays locked tracks and vias in mechanical coordinates."""

    def __init__(self, board):
        self.board = board
        self.nets = {n.GetNetname(): n for n in board.GetNetsByName().values()}
        self.fps = {fp.GetReference(): fp for fp in board.GetFootprints()}
        self.count = {'tracks': 0, 'vias': 0}

    def pad(self, ref, number):
        for p in self.fps[ref].Pads():
            if p.GetNumber() == str(number):
                q = p.GetPosition()
                return (q.x / 1e6 - 100.0, 100.0 - q.y / 1e6)
        raise KeyError(f'{ref}.{number}')

    def track(self, net, points, width, layer=pcbnew.F_Cu):
        for a, b in zip(points, points[1:]):
            if abs(a[0] - b[0]) < 1e-6 and abs(a[1] - b[1]) < 1e-6:
                continue
            t = pcbnew.PCB_TRACK(self.board)
            t.SetStart(to_kicad(a))
            t.SetEnd(to_kicad(b))
            t.SetWidth(mm(width))
            t.SetLayer(layer)
            t.SetNet(self.nets[net])
            t.SetLocked(True)
            self.board.Add(t)
            self.count['tracks'] += 1

    def via(self, net, xy, size=None):
        size = size or SIGNAL_VIA
        v = pcbnew.PCB_VIA(self.board)
        v.SetViaType(pcbnew.VIATYPE_THROUGH)
        v.SetPosition(to_kicad(xy))
        v.SetWidth(mm(size[0]))
        v.SetDrill(mm(size[1]))
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(self.nets[net])
        v.SetLocked(True)
        self.board.Add(v)
        self.count['vias'] += 1

    def gnd_via(self, xy, pad_xy=None):
        """GND via, optionally tied to a pad with a short 0.3 mm track."""
        self.via('GND', xy)
        if pad_xy:
            self.track('GND', [pad_xy, xy], 0.3)


def add(a, b):
    return (a[0] + b[0], a[1] + b[1])


# ---------------------------------------------------------------- RF feed
def route_rf(c):
    w = impedance.RF_WIDTH
    pin1 = c.pad('U1', 1)
    c18, l3a, l3b, c19 = c.pad('C18', 1), c.pad('L3', 1), c.pad('L3', 2), c.pad('C19', 1)
    c25, r20a, r20b, c26 = c.pad('C25', 1), c.pad('R20', 1), c.pad('R20', 2), c.pad('C26', 1)
    ant = c.pad('AE1', 1)
    # Pin 1 leaves +Y, turns 45 degrees and lands on the shunt pad in line.
    knee = (pin1[0], pin1[1] + 0.55)
    run = c18[1] - knee[1]
    c.track('LNA_IN', [pin1, knee, (knee[0] + run, c18[1]), c18, l3a], w)
    # Chip match -> straight up 1 mm right of C19 -> along the antenna row
    # through the antenna match. Both bends are 45 degrees, clear of the shunt
    # ground pads.
    x = c19[0] + 1.0
    c.track('ANT_FEED', [l3b, c19, (x - 0.5, c19[1]), (x, c19[1] + 0.5),
                         (x, c25[1] - 0.5), (x + 0.5, c25[1]), c25, r20a], w)
    c.track('ANT', [r20b, c26, (ant[0], c26[1])], w)
    # Chip-match shunts return up, antenna-match shunts down (the board edge is above).
    for ref, dy in (('C18', 0.7), ('C19', 0.7), ('C25', -0.7), ('C26', -0.7)):
        g = c.pad(ref, 2)
        c.gnd_via((g[0], g[1] + dy), g)
    # Via fence 0.8 mm off both sides of the line, outside the antenna keepout.
    for dy in (2.4, 3.6):
        c.via('GND', (x - 0.8, c19[1] + dy))
    for dy in (1.2, 2.4):
        c.via('GND', (x + 0.8, c19[1] + dy))
    for dx in (0.5, 1.5, 2.5):
        c.via('GND', (c25[0] + dx, c25[1] + 0.8))
    c.via('GND', (r20a[0] + 0.5, c25[1] - 0.9))


# ----------------------------------------------------------- crystals/VDD3P3
def route_clocks(c):
    # 40 MHz: pin 30 -> L2 -> Y1.1 (+C14), pin 29 -> between the Y1 pads -> Y1.3 (+C15).
    p30, p29 = c.pad('U1', 30), c.pad('U1', 29)
    l2a, l2b = c.pad('L2', 2), c.pad('L2', 1)
    y1, y3 = c.pad('Y1', 1), c.pad('Y1', 3)
    c14, c15 = c.pad('C14', 1), c.pad('C15', 1)
    c.track('XTAL_P', [p30, (p30[0] + 0.5, p30[1]), (l2a[0] - 0.2, l2a[1]), l2a], 0.15)
    c.track('X40_P', [l2b, (l2b[0] + (y1[1] - l2b[1]), y1[1]), y1], 0.15)
    c.track('X40_P', [c14, (y1[0], c14[1] - 0.3), y1], 0.15)
    channel = (y1[1] + c.pad('Y1', 2)[1]) / 2
    c.track('XTAL_N', [p29, (p29[0] + 0.45, channel), (y3[0], channel), y3], 0.15)
    c.track('XTAL_N', [y3, (c15[0] - 0.4, c15[1]), c15], 0.15)
    c.gnd_via(add(c.pad('Y1', 2), (0.0, -1.05)), c.pad('Y1', 2))
    c.gnd_via(add(c.pad('Y1', 4), (0.9, 0.5)), c.pad('Y1', 4))
    c.gnd_via(add(c.pad('C14', 2), (0.65, 0.0)), c.pad('C14', 2))
    c.gnd_via(add(c.pad('C15', 2), (0.7, 0.0)), c.pad('C15', 2))

    # 32.768 kHz straight up from pins 4/5; X32_N doglegs left above the EN parts.
    p4, p5 = c.pad('U1', 4), c.pad('U1', 5)
    ya, yb = c.pad('Y2', 1), c.pad('Y2', 2)
    c.track('X32_P', [p4, (p4[0], ya[1] - 1.2), (ya[0], ya[1] - 0.6), ya], 0.15)
    c.track('X32_N', [p5, (p5[0], 9.7), (yb[0], 9.7), yb], 0.15)
    c.track('X32_P', [ya, c.pad('C16', 1)], 0.15)
    c.track('X32_N', [yb, c.pad('C17', 1)], 0.15)
    g16, g17 = c.pad('C16', 2), c.pad('C17', 2)
    mid = ((g16[0] + g17[0]) / 2, g16[1])
    c.gnd_via(mid)
    c.track('GND', [g16, mid, g17], 0.3)

    # VDD3P3: tie pins 2/3, straight up through C8 to L1; L1 top goes to the +3V3 plane.
    p2, p3 = c.pad('U1', 2), c.pad('U1', 3)
    bar_y = p2[1] + 0.5
    x0 = (p2[0] + p3[0]) / 2
    c.track('VDD3P3', [p3, (p3[0], bar_y), (p2[0], bar_y), p2], 0.2)
    c.track('VDD3P3', [(x0, bar_y), c.pad('C8', 1), c.pad('L1', 2)], design.POWER_NETS['VDD3P3'])
    l1 = c.pad('L1', 1)
    c.via('+3V3', (l1[0], l1[1] + 0.75), POWER_VIA)
    c.track('+3V3', [l1, (l1[0], l1[1] + 0.75)], 0.4)
    g8 = c.pad('C8', 2)
    c.gnd_via((g8[0] + 0.05, g8[1] + 0.8), g8)


# ---------------------------------------------------------------------- USB
def route_usb(c):
    w = 0.2
    dp, dn = c.pad('U1', 26), c.pad('U1', 25)
    e_dp, e_dn = c.pad('U6', 1), c.pad('U6', 3)
    # C3 -> ESD left pins: parallel 45 degree drop below the crystal.
    c.track('USB_DP', [dp, (dp[0] + 0.85, dp[1]), (dp[0] + 0.85 + dp[1] - e_dp[1], e_dp[1]), e_dp], w)
    c.track('USB_DN', [dn, (dn[0] + 0.65, dn[1]), (dn[0] + 0.65 + dn[1] - e_dn[1], e_dn[1]), e_dn], w)
    # ESD centre GND pin drops straight into L2.
    g = c.pad('U6', 2)
    c.gnd_via((g[0] - 1.05, g[1]), g)

    # ESD right pins -> receptacle. B6/B7 are direct; A6 (D+) loops round the A7 via,
    # and A7 (D-) dives to B.Cu and comes back up on the D- run.
    o_dp, o_dn = c.pad('U6', 6), c.pad('U6', 4)
    a6, a7, b6, b7 = (c.pad('J1', n) for n in ('A6', 'A7', 'B6', 'B7'))
    loop_x = 17.05
    c.track('USB_DP', [o_dp, (o_dp[0] + 0.65, o_dp[1]), (o_dp[0] + 1.1, b6[1]), b6], w)
    c.track('USB_DP', [a6, (loop_x, a6[1]), (loop_x, b6[1])], w)
    c.track('USB_DN', [o_dn, (o_dn[0] + 0.7, b7[1]), b7], w)
    via_a7 = (17.6, a7[1])
    via_dn = (16.6, b7[1])
    c.track('USB_DN', [a7, via_a7], w)
    c.via('USB_DN', via_a7)
    c.via('USB_DN', via_dn)
    c.track('USB_DN', [via_a7, via_dn], w, pcbnew.B_Cu)

    # VBUS: lower pad pair feeds C1 on F.Cu; the upper pair and the ESD rail join on B.Cu.
    vb_lo, vb_hi = c.pad('J1', 'A4'), c.pad('J1', 'A9')
    c1 = c.pad('C1', 1)
    v_lo, v_hi = (17.75, vb_lo[1]), (17.75, vb_hi[1])
    c.track('VBUS', [vb_lo, v_lo, c1], 0.5)
    c.track('VBUS', [vb_hi, v_hi], 0.5)
    c.via('VBUS', v_lo, POWER_VIA)
    c.via('VBUS', v_hi, POWER_VIA)
    c.track('VBUS', [v_hi, (18.6, 1.6), (18.6, -1.6), v_lo], 0.5, pcbnew.B_Cu)
    esd = c.pad('U6', 5)
    v_esd = (14.4, esd[1])
    c.track('VBUS', [esd, v_esd], 0.3)
    c.via('VBUS', v_esd, POWER_VIA)
    c.track('VBUS', [v_esd, (v_esd[0], -1.6), (v_esd[0] + 0.85, v_lo[1]), v_lo], 0.3, pcbnew.B_Cu)


# ------------------------------------------------------------- MCU locals
def route_mcu(c):
    # +3V3 pins 31/32 tied at the pad tips, then C10 and C9 in a column.
    p31, p32 = c.pad('U1', 31), c.pad('U1', 32)
    c10, c9 = c.pad('C10', 1), c.pad('C9', 1)
    tip = p31[0] + 0.5
    c.track('+3V3', [p31, (tip, p31[1]), (tip, p32[1]), p32], 0.2)
    c.track('+3V3', [(tip, p32[1]), (c10[0] - 0.3, c10[1]), c10, c9], 0.25)
    v = (c10[0] - 0.62, c10[1] + 0.55)
    c.via('+3V3', v, POWER_VIA)
    c.track('+3V3', [v, (c10[0], v[1])], 0.3)
    g10, g9 = c.pad('C10', 2), c.pad('C9', 2)
    gv = (g10[0] + 0.57, (g10[1] + g9[1]) / 2)
    c.gnd_via(gv)
    c.track('GND', [g10, gv, g9], 0.3)

    # Exposed pad: nine vias (Espressif minimum) at 0.7 mm pitch along the
    # paste-free cross (exception to no via-in-pad).
    ep = c.pad('U1', 33)
    for dx, dy in ((0, 0), (0.7, 0), (-0.7, 0), (1.4, 0), (-1.4, 0),
                   (0, 0.7), (0, -0.7), (0, 1.4), (0, -1.4)):
        c.via('GND', (ep[0] + dx, ep[1] + dy))

    # LED_DIN and KEY_NEXT escape to B.Cu between the USB pair and XTAL_N.
    p27, p28 = c.pad('U1', 27), c.pad('U1', 28)
    v27, v28 = (9.8, 3.2), (11.0, 3.55)
    c.track('LED_DIN', [p27, (p27[0] + 0.9, p27[1]), v27], 0.15)
    c.track('KEY_NEXT_MCU', [p28, (10.7, p28[1]), v28], 0.15)
    c.via('LED_DIN', v27)
    c.via('KEY_NEXT_MCU', v28)

    # CHRG (pin 16) drops to B.Cu below the pin before the key lines fan out
    # towards the main-key end and wall it in.
    p16, v16 = c.pad('U1', 16), (2.8, 1.1)
    c.track('CHRG', [p16, (3.4, 2.1), (2.8, 1.5), v16], 0.15)
    c.via('CHRG', v16)

    # VDD_SPI: pin 18 straight down to flash pin 8, then along the pad bottoms
    # to C20/C21. Pin 17 (+3V3) keeps the room on its left for C12.
    p18, f8 = c.pad('U1', 18), c.pad('U4', 8)
    c20, c21 = c.pad('C20', 1), c.pad('C21', 1)
    knee = (p18[0], f8[1] + 1.41)
    c.track('VDD_SPI', [p18, knee, (f8[0] + 0.5, f8[1] + 0.86), f8], 0.25)
    c.track('VDD_SPI', [f8, (f8[0], c20[1]), c20, c21], 0.25)

    # Left side: pin 11 (+3V3) taps the plane right at its tip and pin 10
    # (VBUS_SENSE) drops to B.Cu one pitch further out, so pins 9 and 12-16
    # still leave in straight lines between the two vias.
    p10, p11 = c.pad('U1', 10), c.pad('U1', 11)
    v11, v10 = (p11[0] - 1.0, p11[1]), (p10[0] - 1.95, p10[1])
    c.track('+3V3', [p11, v11], 0.25)
    c.via('+3V3', v11)
    c.track('VBUS_SENSE', [p10, v10], 0.15)
    c.via('VBUS_SENSE', v10)


# ---------------------------------------------------------------- SPI flash
def route_flash(c):
    """U1 pins 19-24 to the SOIC-8 flash right below it (KRT kept failing here).

    HD, CLK and D drop straight onto the top pad row. WP threads between pads 7
    and 6 and crosses under the package body to pad 3. CS and Q cross the other
    lines on B.Cu and come back up under the body to pads 1 and 2.
    """
    pad_half = 1.625 / 2   # SOIC pad length along Y

    def drop(net, pin, pad, top_row=True):
        # Straight down from the pin, then 45 degrees onto the pad's inner end.
        edge = pad[1] + pad_half if top_row else pad[1] - pad_half
        knee_y = edge + abs(pad[0] - pin[0])
        c.track(net, [pin, (pin[0], knee_y), (pad[0], edge), pad], 0.15)

    drop('SPI_HD', c.pad('U1', 19), c.pad('U4', 7))
    drop('SPI_CLK', c.pad('U1', 22), c.pad('U4', 6))
    drop('SPI_D', c.pad('U1', 23), c.pad('U4', 5))

    p20, f3 = c.pad('U1', 20), c.pad('U4', 3)
    lane = (c.pad('U4', 7)[0] + f3[0]) / 2   # centre of the gap between pads 7 and 6
    under = f3[1] + pad_half
    c.track('SPI_WP', [p20, (p20[0], 0.8), (lane, 0.8 - (p20[0] - lane)), (lane, under + (f3[0] - lane)),
                       (f3[0], under), f3], 0.15)

    def cross(net, pin, pad, via_x, via_y):
        # Pin -> via just below U1 -> B.Cu diagonal -> via under the body -> pad.
        v1 = (pin[0], 0.35)
        v2 = (via_x, via_y)
        c.track(net, [pin, v1], 0.15)
        c.via(net, v1)
        diagonal_top = v2[1] + (v1[0] - v2[0])
        c.track(net, [v1, (v1[0], diagonal_top), v2], 0.15, pcbnew.B_Cu)
        c.via(net, v2)
        edge = pad[1] + pad_half
        c.track(net, [v2, (v2[0], edge + abs(pad[0] - v2[0])), (pad[0], edge), pad], 0.15)
        return v2

    f1, f2 = c.pad('U4', 1), c.pad('U4', 2)
    cs_via = cross('SPI_CS', c.pad('U1', 21), f1, f1[0] + 0.635, -3.0)
    cross('SPI_Q', c.pad('U1', 24), f2, f2[0], -4.6)

    # CS pull-up R12: its CS pad joins the CS lane under the body, its VDD_SPI
    # pad runs left and up to C21.
    r12_cs, r12_vdd, c21 = c.pad('R12', 2), c.pad('R12', 1), c.pad('C21', 1)
    c.track('SPI_CS', [r12_cs, (cs_via[0], r12_cs[1])], 0.15)
    c.track('VDD_SPI', [r12_vdd, (c21[0], r12_vdd[1]), c21], 0.25)

# ------------------------------------------------------------ power path
def route_power(c):
    """Charger, power path and LDO trunks (VBUS, VSYS, VBAT, +3V3 at the LDO)."""
    # VBUS: the B.Cu feed from the receptacle drops to a via between the
    # charger VCC pin and the Schottky anode, then thin branches to the
    # power-path gate, its pull-down and the sense divider.
    u2_vcc, d1_a = c.pad('U2', 4), c.pad('D1', 2)
    v_vbus = (d1_a[0] + 0.15, -6.4)
    c.track('VBUS', [(15.25, -2.45), (15.25, -4.75), v_vbus], 0.5, pcbnew.B_Cu)
    c.via('VBUS', v_vbus, POWER_VIA)
    c.track('VBUS', [u2_vcc, (u2_vcc[0], v_vbus[1]), v_vbus, (d1_a[0], v_vbus[1]), d1_a], 0.5)
    gate, r5, r8 = c.pad('Q1', 1), c.pad('R5', 1), c.pad('R8', 1)
    bend_y = -8.65
    c.track('VBUS', [d1_a, (d1_a[0], bend_y), (gate[0] + 0.69, bend_y),
                     (gate[0], bend_y - 0.69), gate], 0.3)
    c.track('VBUS', [gate, (gate[0] - 0.66, r5[1]), r5], 0.3)
    c.track('VBUS', [(d1_a[0], -8.4), (r8[0] - 0.4, -8.4), (r8[0], -8.8), r8], 0.3)

    # VSYS: Schottky cathode down the left of R5 to the PMOS source and the
    # LDO input, then around U3's ground pin to EN (pin 3) and C3.
    d1_k, q1_s = c.pad('D1', 1), c.pad('Q1', 2)
    ldo_in, ldo_en, c3 = c.pad('U3', 1), c.pad('U3', 3), c.pad('C3', 1)
    x_run = 8.3
    c.track('VSYS', [d1_k, (x_run, d1_k[1]), (x_run, -11.0), (x_run + 0.5, -11.5),
                     (q1_s[0], -11.5)], 0.5)
    c.track('VSYS', [(x_run, -11.0), (ldo_in[0] + 0.64, -11.0), (ldo_in[0], -11.64), ldo_in], 0.5)
    x_left = 5.35
    c.track('VSYS', [ldo_in, (x_left, ldo_in[1]), (x_left, ldo_en[1]), ldo_en], 0.5)
    c.track('VSYS', [(x_left, c3[1]), c3], 0.5)

    # +3V3: LDO output to C4, with a power via into the In2.Cu plane on the way.
    ldo_out, c4 = c.pad('U3', 5), c.pad('C4', 1)
    knee = (ldo_out[0] + 0.86, ldo_out[1])
    c.track('+3V3', [ldo_out, knee, (c4[0], knee[1] - (c4[0] - knee[0])), c4], 0.4)
    c.via('+3V3', (knee[0] + 0.4, knee[1] - 0.4), POWER_VIA)

    # VBAT: battery pad up through a via, along B.Cu to the PMOS drain and to
    # the charger BAT pin / C2 pair.
    j2, q1_d = c.pad('J2', 1), c.pad('Q1', 3)
    u2_bat, c2 = c.pad('U2', 3), c.pad('C2', 1)
    v_j2 = (j2[0], -12.3)
    v_q1 = (q1_d[0], q1_d[1] - 0.95)
    v_c2 = (10.45, -6.45)
    c.track('VBAT', [j2, v_j2], 0.5)
    c.track('VBAT', [q1_d, v_q1], 0.5)
    c.track('VBAT', [c2, (c2[0], u2_bat[1]), u2_bat], 0.5)
    c.track('VBAT', [(v_c2[0], u2_bat[1]), v_c2], 0.5)
    for v in (v_j2, v_q1, v_c2):
        c.via('VBAT', v, POWER_VIA)
    run_y = -11.0
    c.track('VBAT', [v_j2, (v_j2[0] + 1.3, run_y), (v_q1[0] - 0.55, run_y), v_q1], 0.5, pcbnew.B_Cu)
    c.track('VBAT', [(v_c2[0], run_y), v_c2], 0.5, pcbnew.B_Cu)

    # VBUS_SENSE: from the U1 escape via straight down B.Cu, left of the flash
    # crossovers (route_flash), under the flash body, between the VBAT and VBUS
    # trunks, up to the divider node between R8 and R9.
    r8_mid, r9_top = c.pad('R8', 2), c.pad('R9', 1)
    start = (c.pad('U1', 10)[0] - 1.95, c.pad('U1', 10)[1])
    node_y = (r8_mid[1] + r9_top[1]) / 2
    v_div = (r8_mid[0] - 0.9, node_y)
    lane_x, lane_y = 11.8, -5.6
    c.track('VBUS_SENSE', [start, (start[0], lane_y + 0.6), (start[0] + 0.6, lane_y), (lane_x, lane_y),
                           (lane_x, -10.2), (13.0, -10.2), (v_div[0] - 0.4, v_div[1]), v_div],
            0.15, pcbnew.B_Cu)
    c.via('VBUS_SENSE', v_div)
    c.track('VBUS_SENSE', [v_div, (r8_mid[0], node_y)], 0.15)
    c.track('VBUS_SENSE', [r8_mid, r9_top], 0.15)


def save(board, src, dst):
    pcbnew.SaveBoard(str(dst), board)
    for suffix in ('.kicad_pro', '.kicad_dru'):
        shutil.copyfile(src.with_suffix(suffix), dst.with_suffix(suffix))


def main():
    pcbnew.KIID.SeedGenerator(design.KIID_SEEDS['route_critical'])
    src = BUILD / '01_placed.kicad_pcb'
    dst = BUILD / '02_critical.kicad_pcb'
    board = pcbnew.LoadBoard(str(src))
    c = Copper(board)
    route_rf(c)
    route_clocks(c)
    route_usb(c)
    route_mcu(c)
    route_flash(c)
    route_power(c)
    save(board, src, dst)
    print(f"critical copper: {c.count['tracks']} tracks, {c.count['vias']} vias -> {dst}")


if __name__ == '__main__':
    main()
