#!/usr/bin/env python3
"""Enlarge every high-current via to 0.6/0.3 mm after KRT routing.

KRT routes one via size per run, and routing the power nets first with large
vias blocks the MCU escape. So the whole board is routed with 0.45/0.3 vias and
this script grows the power vias afterwards. A via that cannot grow in place is
nudged (up to MAX_NUDGE mm) together with the track ends attached to it.
Anything that still does not fit is reported and the script exits non-zero,
or, with --rip-failed, ripped out with its branch so KRT can re-route that gap
with --via-size 0.6 --via-drill 0.3.

Run with KiCad's bundled python:
    python3 tools/upsize_power_vias.py IN.kicad_pcb OUT.kicad_pcb [--rip-failed]
"""
import math
import shutil
import sys
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design  # noqa: E402

POWER_VIA = (0.6, 0.3)
HOLE_TO_HOLE = 0.25
EDGE_CLEARANCE = 0.3
SAME_NET_PAD_GAP = 0.1      # keeps vias out of same-net pads (no via-in-pad)
MAX_NUDGE = 0.8
COPPER = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]


def mm(v):
    return pcbnew.FromMM(v)


def uid(item):
    return item.m_Uuid.AsString()


def to_mech(p):
    return (round(pcbnew.ToMM(p.x) - 100.0, 2), round(100.0 - pcbnew.ToMM(p.y), 2))


class Checker:
    """Clearance checks for one via (plus its attached tracks) against the board."""

    def __init__(self, board):
        self.board = board
        self.tracks = [t for t in board.GetTracks()]
        self.pads = [p for fp in board.GetFootprints() for p in fp.Pads()]
        self.edges = [d for d in board.GetDrawings() if d.GetLayer() == pcbnew.Edge_Cuts]
        self.keepouts = [z for z in board.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowVias()]

    @staticmethod
    def gap(a, b, layer):
        return max(a.GetOwnClearance(layer), b.GetOwnClearance(layer))

    def copper_ok(self, item, layers, ignore):
        """True if `item` clears every other-net track, via and pad on `layers`."""
        net = item.GetNetCode()
        for layer in layers:
            shape = item.GetEffectiveShape(layer)
            for other in self.tracks:
                if uid(other) in ignore or other.GetNetCode() == net or not other.IsOnLayer(layer):
                    continue
                if shape.Collide(other.GetEffectiveShape(layer), self.gap(item, other, layer)):
                    return False
            for pad in self.pads:
                if not pad.IsOnLayer(layer):
                    continue
                if pad.GetNetCode() == net:
                    # Same-net SMD pads: forbid via-in-pad for the via itself.
                    if item.GetClass() == 'PCB_VIA' and pad.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                        if shape.Collide(pad.GetEffectiveShape(layer), mm(SAME_NET_PAD_GAP)):
                            return False
                    continue
                if shape.Collide(pad.GetEffectiveShape(layer), self.gap(item, pad, layer)):
                    return False
        return True

    def hole_ok(self, via, ignore):
        c = via.GetPosition()
        r = via.GetDrill() / 2
        holes = [(t.GetPosition(), t.GetDrill() / 2) for t in self.tracks
                 if t.GetClass() == 'PCB_VIA' and uid(t) not in ignore]
        holes += [(p.GetPosition(), p.GetDrillSize().x / 2) for p in self.pads
                  if p.GetDrillSize().x > 0]
        for pos, rr in holes:
            if math.hypot(pos.x - c.x, pos.y - c.y) - r - rr < mm(HOLE_TO_HOLE):
                return False
        return True

    def edge_ok(self, via):
        shape = via.GetEffectiveShape(pcbnew.F_Cu)
        return not any(shape.Collide(e.GetEffectiveShape(), mm(EDGE_CLEARANCE)) for e in self.edges)

    def keepout_ok(self, via):
        pos = via.GetPosition()
        return not any(z.HitTestFilledArea(pcbnew.F_Cu, pos) or z.Outline().Contains(pos)
                       for z in self.keepouts)


def attached_tracks(board, via):
    pos = via.GetPosition()
    out = []
    for t in board.GetTracks():
        if t.GetClass() != 'PCB_TRACK' or t.GetNetCode() != via.GetNetCode():
            continue
        if t.GetStart() == pos:
            out.append((t, 'start'))
        elif t.GetEnd() == pos:
            out.append((t, 'end'))
    return out


def try_position(chk, via, ends, pos):
    """Move via and attached ends to pos; keep it if every check passes."""
    old = via.GetPosition()
    via.SetPosition(pos)
    for t, side in ends:
        (t.SetStart if side == 'start' else t.SetEnd)(pos)
    ignore = {uid(via)} | {uid(t) for t, _ in ends}
    ok = (chk.copper_ok(via, COPPER, ignore) and chk.hole_ok(via, ignore)
          and chk.edge_ok(via) and chk.keepout_ok(via)
          and all(chk.copper_ok(t, [t.GetLayer()], ignore) for t, _ in ends))
    if not ok:
        via.SetPosition(old)
        for t, side in ends:
            (t.SetStart if side == 'start' else t.SetEnd)(old)
    return ok


def candidate_offsets():
    yield (0.0, 0.0)
    step = 0.05
    r = step
    while r <= MAX_NUDGE + 1e-9:
        for k in range(16):
            a = 2 * math.pi * k / 16
            yield (r * math.cos(a), r * math.sin(a))
        r += step


def upsize(board):
    chk = Checker(board)
    grown, nudged, failed = [], [], []
    vias = [t for t in board.GetTracks() if t.GetClass() == 'PCB_VIA'
            and t.GetNetname() in design.HIGH_CURRENT_NETS]
    for via in vias:
        if pcbnew.ToMM(via.GetWidth(pcbnew.F_Cu)) >= POWER_VIA[0] - 1e-6:
            continue
        old_w, old_d = via.GetWidth(pcbnew.F_Cu), via.GetDrill()
        via.SetWidth(mm(POWER_VIA[0]))
        via.SetDrill(mm(POWER_VIA[1]))
        ends = attached_tracks(board, via)
        origin = via.GetPosition()
        label = f'{via.GetNetname()} {to_mech(origin)}'
        for dx, dy in candidate_offsets():
            pos = pcbnew.VECTOR2I(origin.x + mm(dx), origin.y + mm(dy))
            if try_position(chk, via, ends, pos):
                (grown if (dx, dy) == (0.0, 0.0) else nudged).append(
                    label if (dx, dy) == (0.0, 0.0) else f'{label} moved {math.hypot(dx, dy):.2f}')
                break
        else:
            via.SetWidth(old_w)
            via.SetDrill(old_d)
            failed.append(label)
    return grown, nudged, failed


def rip_branch(board, via):
    """Delete a via and the unlocked tracks around it up to the nearest pad,
    junction or other via, so KRT can re-route the gap with power vias."""
    net = via.GetNetCode()
    tracks = [t for t in board.GetTracks()
              if t.GetClass() == 'PCB_TRACK' and t.GetNetCode() == net]
    stops = {(p.GetPosition().x, p.GetPosition().y)
             for fp in board.GetFootprints() for p in fp.Pads() if p.GetNetCode() == net}
    stops |= {(t.GetPosition().x, t.GetPosition().y) for t in board.GetTracks()
              if t.GetClass() == 'PCB_VIA' and t.GetNetCode() == net and uid(t) != uid(via)}

    def degree(pt):
        return sum(1 for t in tracks if (t.GetStart().x, t.GetStart().y) == pt
                   or (t.GetEnd().x, t.GetEnd().y) == pt)

    removed, frontier = set(), [(via.GetPosition().x, via.GetPosition().y)]
    while frontier:
        pt = frontier.pop()
        for t in tracks:
            if uid(t) in removed or t.IsLocked():
                continue
            a, b = (t.GetStart().x, t.GetStart().y), (t.GetEnd().x, t.GetEnd().y)
            if pt not in (a, b):
                continue
            removed.add(uid(t))
            nxt = b if pt == a else a
            if nxt not in stops and degree(nxt) == 2:
                frontier.append(nxt)
    for t in tracks:
        if uid(t) in removed:
            board.Delete(t)
    board.Delete(via)
    return len(removed)


def main():
    pcbnew.KIID.SeedGenerator(design.KIID_SEEDS['upsize_power_vias'])
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    rip = '--rip-failed' in sys.argv
    src, dst = Path(args[0]), Path(args[1])
    board = pcbnew.LoadBoard(str(src))
    grown, nudged, failed = upsize(board)
    ripped_nets = set()
    if rip and failed:
        small = [uid(t) for t in board.GetTracks() if t.GetClass() == 'PCB_VIA'
                 and t.GetNetname() in design.HIGH_CURRENT_NETS
                 and pcbnew.ToMM(t.GetWidth(pcbnew.F_Cu)) < POWER_VIA[0] - 1e-6]
        for via_id in small:
            # Look the via up again: removing items invalidates older wrappers.
            via = next(t for t in board.GetTracks() if uid(t) == via_id)
            label = f'{via.GetNetname()} {to_mech(via.GetPosition())}'
            ripped_nets.add(via.GetNetname())
            print(f'  ripped {label} with {rip_branch(board, via)} track(s)')
    # make.sh re-routes exactly these nets with power-size vias.
    dst.with_suffix('.ripped').write_text(''.join(f'{n}\n' for n in sorted(ripped_nets)))
    board.Save(str(dst))
    for ext in ('.kicad_pro', '.kicad_dru'):
        if src.with_suffix(ext).exists():
            shutil.copy(src.with_suffix(ext), dst.with_suffix(ext))
    print(f'power vias: {len(grown)} grown in place, {len(nudged)} nudged, {len(failed)} failed')
    for line in nudged:
        print('  nudged', line)
    if not rip:
        for line in failed:
            print('  FAILED', line)
    sys.exit(0 if rip or not failed else 1)


if __name__ == '__main__':
    main()
