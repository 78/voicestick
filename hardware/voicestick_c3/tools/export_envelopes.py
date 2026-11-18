#!/usr/bin/env python3
"""Export component envelopes of the finished board for the enclosure script.

Run with KiCad's bundled python:
    python3 tools/export_envelopes.py kicad/voicestick-c3-v1.kicad_pcb
Writes enclosure/pcb-envelopes.json (mechanical coordinates, mm):
  components  XY box of each part (fab outline + pads) and its height above the PCB top
  tht         through-hole leads that stick out under the board (floor relief)
  holes       mounting holes
The main switch is modelled from assembly-interface.json instead (it is not a box).
"""
import json
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'enclosure' / 'pcb-envelopes.json'

# Maximum body height above the PCB top, from the part datasheets.
HEIGHTS = {
    'Alps_SKRTLBE010': 3.3,
    'C_0402_1005Metric': 0.55,
    'C_0603_1608Metric': 0.9,
    'ChipAntenna_3216': 1.2,
    'Crystal_SMD_3215-2Pin_3.2x1.5mm': 0.9,
    'Crystal_SMD_3225-4Pin_3.2x2.5mm': 0.8,
    'D_SOD-123': 1.35,
    'Goertek_SD18OB371-041': 1.0,
    'JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal': 2.95,
    'LED_0402_1005Metric': 0.4,
    'LED_LiteOn_LTST-S270': 1.2,
    'LED_WS2812B-2020_PLCC4_2.0x2.0mm': 0.84,
    'L_0402_1005Metric': 0.55,
    'QFN-32-1EP_5x5mm_P0.5mm_EP3.7x3.7mm': 0.9,
    'R_0402_1005Metric': 0.4,
    'SOIC-8_5.3x5.3mm_P1.27mm': 2.16,
    'SOT-23': 1.2,
    'SOT-23-5': 1.45,
    'SOT-23-6': 1.45,
    'USB_C_Receptacle_HRO_TYPE-C-31-M-12': 3.26,
}
SKIP = {'CPG151101D05', 'MountingHole_2.2mm_M2', 'TestPoint_Pad_D1.0mm'}
# Lead length under the PCB bottom for through-hole parts.
LEAD_BELOW = {'CPG151101D05': 1.85, 'USB_C_Receptacle_HRO_TYPE-C-31-M-12': 1.0}


def mech_box(box):
    return [round(pcbnew.ToMM(box.GetLeft()) - 100.0, 3), round(pcbnew.ToMM(box.GetRight()) - 100.0, 3),
            round(100.0 - pcbnew.ToMM(box.GetBottom()), 3), round(100.0 - pcbnew.ToMM(box.GetTop()), 3)]


def main():
    board = pcbnew.LoadBoard(sys.argv[1])
    components, tht, holes = [], [], []
    for fp in board.GetFootprints():
        name = str(fp.GetFPID().GetLibItemName())
        ref = fp.GetReference()
        if name == 'MountingHole_2.2mm_M2':
            p = fp.GetPosition()
            holes.append({'ref': ref, 'xy': [round(pcbnew.ToMM(p.x) - 100.0, 3),
                                             round(100.0 - pcbnew.ToMM(p.y), 3)]})
        for pad in fp.Pads():
            if pad.GetDrillSize().x <= 0 or name not in LEAD_BELOW:
                continue
            p = pad.GetPosition()
            tht.append({'ref': ref, 'pad': pad.GetNumber(),
                        'xy': [round(pcbnew.ToMM(p.x) - 100.0, 3), round(100.0 - pcbnew.ToMM(p.y), 3)],
                        'drill': [round(pcbnew.ToMM(pad.GetDrillSize().x), 3),
                                  round(pcbnew.ToMM(pad.GetDrillSize().y), 3)],
                        'angle': round(pad.GetOrientationDegrees(), 1),
                        'below_pcb': LEAD_BELOW[name]})
        if name in SKIP:
            continue
        assert name in HEIGHTS, f'no height for {name} ({ref})'
        box = pcbnew.BOX2I()
        for item in fp.GraphicalItems():
            if item.GetLayer() == pcbnew.F_Fab and item.GetClass() != 'PCB_TEXT':
                box.Merge(item.GetBoundingBox())
        for pad in fp.Pads():
            box.Merge(pad.GetBoundingBox())
        components.append({'ref': ref, 'footprint': name, 'box': mech_box(box), 'height': HEIGHTS[name]})
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_text(json.dumps({'components': sorted(components, key=lambda c: c['ref']),
                               'tht': tht, 'holes': sorted(holes, key=lambda h: h['ref'])}, indent=1))
    print(f'envelopes written: {OUT} ({len(components)} parts, {len(tht)} leads, {len(holes)} holes)')


if __name__ == '__main__':
    main()
