#!/usr/bin/env python3
"""Quality figures of a finished board, used to compare routing variants.

Run with KiCad's bundled python:
    python3 tools/board_metrics.py kicad/voicestick-c3-v1.kicad_pcb
Prints one JSON object. Lower is better except gnd_outer_area_mm2.
"""
import json
import sys

import pcbnew

# Requested widths of the KRT power nets (same as POWER in make.sh) and of the
# hand-routed trunks; copper narrower than this is a neck.
POWER_WIDTHS = {'+3V3': 0.4, 'VDD3P3': 0.5, 'PERIPH_3V3': 0.3, 'MIC_VDD': 0.25, 'VDD_SPI': 0.25}


def main():
    board = pcbnew.LoadBoard(sys.argv[1])
    signal_vias = gnd_vias = 0
    signal_length = 0.0
    narrow = {}
    for item in board.GetTracks():
        net = item.GetNetname()
        if item.GetClass() == 'PCB_VIA':
            if net == 'GND':
                gnd_vias += 1
            else:
                signal_vias += 1
            continue
        length = pcbnew.ToMM(item.GetLength())
        if net != 'GND':
            signal_length += length
        requested = POWER_WIDTHS.get(net)
        if requested and pcbnew.ToMM(item.GetWidth()) < requested - 1e-3:
            narrow[net] = narrow.get(net, 0.0) + length

    gnd_area = 0.0
    gnd_islands = 0
    for zone in board.Zones():
        if zone.GetNetname() != 'GND':
            continue
        for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
            if not zone.IsOnLayer(layer):
                continue
            fill = zone.GetFilledPolysList(layer)
            gnd_islands += fill.OutlineCount()
            gnd_area += fill.Area() / 1e12  # nm^2 -> mm^2

    print(json.dumps({
        'signal_vias': signal_vias,
        'gnd_vias': gnd_vias,
        'signal_length_mm': round(signal_length, 1),
        'narrow_power_mm': round(sum(narrow.values()), 2),
        'narrow_power_by_net': {k: round(v, 2) for k, v in sorted(narrow.items())},
        'gnd_outer_area_mm2': round(gnd_area, 1),
        'gnd_outer_islands': gnd_islands,
    }))


if __name__ == '__main__':
    main()
