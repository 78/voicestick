#!/usr/bin/env python3
"""Board checks that KiCad's DRC does not express directly.

  - every via meets the JLC no-surcharge size: 0.45/0.3 mm, and 0.6/0.3 mm on the
    high-current nets (design.HIGH_CURRENT_NETS)
  - no via sits in a same-net SMD pad (the U1 exposed pad is the one exception)
  - no via inside the antenna keepout or the microphone port seal
  - the board is 4 layers, 1.0 mm, with the expected outline

Run with KiCad's bundled python:
    python3 tools/check_board.py kicad/voicestick-c3-v1.kicad_pcb
"""
import json
import math
import sys
from collections import Counter
from pathlib import Path

import pcbnew

sys.path.insert(0, str(Path(__file__).resolve().parent))
import design  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
INTERFACE = json.loads((ROOT / 'assembly-interface.json').read_text())
SIGNAL_VIA = (0.45, 0.3)
POWER_VIA = (0.6, 0.3)
EXPOSED_PAD_OK = {('U1', '33')}


def mech(p):
    return (round(pcbnew.ToMM(p.x) - 100.0, 2), round(100.0 - pcbnew.ToMM(p.y), 2))


def main():
    board = pcbnew.LoadBoard(sys.argv[1])
    problems = []

    ds = board.GetDesignSettings()
    if board.GetCopperLayerCount() != 4:
        problems.append(f'copper layers {board.GetCopperLayerCount()} != 4')
    if abs(pcbnew.ToMM(ds.GetBoardThickness()) - INTERFACE['board']['thickness']) > 1e-3:
        problems.append(f'board thickness {pcbnew.ToMM(ds.GetBoardThickness())} mm')
    # The bounding box includes the Edge.Cuts line width.
    box = board.GetBoardEdgesBoundingBox()
    size = (pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight()))
    if any(abs(a - b) > 0.1 for a, b in zip(size, INTERFACE['board']['outline_xy'])):
        problems.append(f'outline {size} != {INTERFACE["board"]["outline_xy"]}')

    vias = [t for t in board.GetTracks() if t.GetClass() == 'PCB_VIA']
    sizes = Counter()
    smd_pads = [(fp.GetReference(), p) for fp in board.GetFootprints() for p in fp.Pads()
                if p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD]
    keepouts = [z for z in board.Zones() if z.GetIsRuleArea() and z.GetDoNotAllowVias()]
    for v in vias:
        w, d = pcbnew.ToMM(v.GetWidth(pcbnew.F_Cu)), pcbnew.ToMM(v.GetDrill())
        need = POWER_VIA if v.GetNetname() in design.HIGH_CURRENT_NETS else SIGNAL_VIA
        sizes[(round(w, 2), round(d, 2))] += 1
        if w < need[0] - 1e-6 or d < need[1] - 1e-6:
            problems.append(f'via {v.GetNetname()} {mech(v.GetPosition())} {w}/{d} < {need}')
        shape = v.GetEffectiveShape(pcbnew.F_Cu)
        for ref, pad in smd_pads:
            if pad.GetNetCode() != v.GetNetCode() or (ref, pad.GetNumber()) in EXPOSED_PAD_OK:
                continue
            if pad.IsOnLayer(pcbnew.F_Cu) and shape.Collide(pad.GetEffectiveShape(pcbnew.F_Cu), 0):
                problems.append(f'via-in-pad {v.GetNetname()} {mech(v.GetPosition())} {ref}.{pad.GetNumber()}')
        for z in keepouts:
            if z.Outline().Contains(v.GetPosition()):
                problems.append(f'via {v.GetNetname()} {mech(v.GetPosition())} inside "{z.GetZoneName()}"')

    print(f'{len(vias)} vias: ' + ', '.join(f'{n}x {w}/{d}' for (w, d), n in sorted(sizes.items())))
    for p in problems:
        print('  FAIL', p)
    print('board checks: ' + ('OK' if not problems else f'{len(problems)} problem(s)'))
    sys.exit(1 if problems else 0)


if __name__ == '__main__':
    main()
