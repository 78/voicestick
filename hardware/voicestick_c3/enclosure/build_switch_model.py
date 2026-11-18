#!/usr/bin/env python3
"""3D model of SW1 (CPG151101D05) for KiCad: kicad/3dmodels/CPG151101D05.step.

Run with FreeCAD's bundled python:
    /Applications/FreeCAD.app/Contents/Resources/bin/python enclosure/build_switch_model.py

LCSC has no model for this part, so it is built from the main_key block of
assembly-interface.json. KiCad frame: footprint origin = switch centre, +Y up,
Z = 0 on the PCB top. Keep the shapes in step with build_main_switch() in
build_enclosure.py, which uses the same dimensions for the interference checks.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, '/Applications/FreeCAD.app/Contents/Resources/lib')
import FreeCAD  # noqa: E402
import Part  # noqa: E402

V = FreeCAD.Vector
ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'kicad' / '3dmodels' / 'CPG151101D05.step'
MK = json.loads((ROOT / 'assembly-interface.json').read_text())['main_key']

# Leads of main_switch_footprint() in tools/build_library.py: (x, y up, drill).
LEADS = [(0.0, 0.0, 4.1), (2.54, 5.08, 1.5), (-3.8, 2.54, 1.5)]


def box(x0, x1, y0, y1, z0, z1):
    return Part.makeBox(x1 - x0, y1 - y0, z1 - z0, V(x0, y0, z0))


def main():
    bx, by = MK['body_xy']
    top = MK['housing_top_above_pcb']
    lower = box(-bx / 2, bx / 2, -by / 2, by / 2, 0, 5.0)
    upper = box(-7.0, 7.0, -7.0, 7.0, 5.0, top)
    window = box(-3.6, 3.6, -2.9, 2.9, top - MK['travel'] - 0.5, top + 1)
    tail = -MK['tail_below_pcb_top']
    leads = [Part.makeCylinder((d - 0.2) / 2, -tail, V(x, y, tail)) for x, y, d in LEADS]
    housing = lower.fuse([upper] + leads).cut(window)

    stem_top = top + MK['stem_above_housing']
    stem = box(-2.0, 2.0, -0.585, 0.585, top, stem_top).fuse(box(-0.585, 0.585, -2.0, 2.0, top, stem_top))

    model = housing.fuse(stem).removeSplitter()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    model.exportStep(str(OUT))
    print('switch model written to', OUT)


if __name__ == '__main__':
    main()
