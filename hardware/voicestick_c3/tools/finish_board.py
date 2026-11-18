#!/usr/bin/env python3
"""Refill every zone of the routed board and install it as the KiCad project board.

Run with KiCad's bundled python:
    python3 tools/finish_board.py build/07_final.kicad_pcb
The board is written to kicad/voicestick-c3-v1.kicad_pcb. Only the .kicad_pcb is
copied: the project file in kicad/ keeps the rules written by build_pcb.py, so
the final DRC runs against our rules and not against KRT's floors.
"""
import shutil
import sys
import tempfile
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
TARGET = ROOT / 'kicad' / 'voicestick-c3-v1.kicad_pcb'


def main():
    board = pcbnew.LoadBoard(sys.argv[1])
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / TARGET.name
        board.Save(str(out))
        shutil.copy(out, TARGET)
    print(f'board installed: {TARGET} ({len(board.Zones())} zones refilled)')


if __name__ == '__main__':
    main()
