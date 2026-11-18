#!/usr/bin/env python3
"""Preview images of the enclosure: an exploded view and two sections.

Run with FreeCAD's python after build_enclosure.py:
    /Applications/FreeCAD.app/Contents/Resources/bin/python enclosure/render_preview.py
Writes enclosure/out/preview-exploded.png and enclosure/out/preview-sections.png.
"""
import sys
from pathlib import Path

sys.path.insert(0, '/Applications/FreeCAD.app/Contents/Resources/lib')
sys.path.insert(0, str(Path(__file__).resolve().parent))
import matplotlib  # noqa: E402

matplotlib.use('Agg')
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.patches import Polygon  # noqa: E402
from mpl_toolkits.mplot3d.art3d import Poly3DCollection  # noqa: E402

import build_enclosure as be  # noqa: E402
import Part  # noqa: E402

STEP_DIR = be.OUT / 'step'
COLORS = {
    'LowerShell': '#9fb7c9', 'UpperShell': '#d9d4c7', 'MainCap': '#e0a96d',
    'SideCap': '#e07a5f', 'PCB': '#2f6b3a', 'Parts': '#333333', 'SW1': '#555555',
    'Battery': '#6a5acd', 'Foam': '#f2cc8f',
}


def load_part(name):
    return Part.read(str(STEP_DIR / f'{name}.step'))


def color_of(name):
    for key, color in COLORS.items():
        if name.startswith(key):
            return color
    return '#888888'


def add_mesh(ax, shape, color, offset=(0, 0, 0), alpha=1.0):
    vertices, triangles = shape.tessellate(0.15)
    dx, dy, dz = offset
    light = be.V(-0.4, -0.6, 0.7).normalize()
    base = matplotlib.colors.to_rgb(color)
    faces, colors = [], []
    for tri in triangles:
        a, b, c = (vertices[i] for i in tri)
        normal = (b - a).cross(c - a)
        if normal.Length < 1e-12:
            continue
        brightness = 0.45 + 0.55 * abs(normal.normalize().dot(light))
        faces.append([(p.x + dx, p.y + dy, p.z + dz) for p in (a, b, c)])
        colors.append((*(channel * brightness for channel in base), alpha))
    ax.add_collection3d(Poly3DCollection(faces, facecolors=colors, edgecolors='none'))


def exploded_view(parts, references, path):
    fig = plt.figure(figsize=(12, 9))
    ax = fig.add_subplot(projection='3d')
    lift = {'UpperShell': 14, 'MainCap': 26, 'Battery': 7}
    for key in be.IF['side_keys']['keys']:
        lift[f"SideCap_{key['ref']}"] = 0
    for name, shape in parts.items():
        offset = (0, 0, lift.get(name, 0))
        if name.startswith('SideCap'):
            side = next(k['side'] for k in be.IF['side_keys']['keys'] if name.endswith(k['ref']))
            offset = (0, side * 6, 0)
        add_mesh(ax, shape, color_of(name), offset, alpha=0.55 if name == 'UpperShell' else 1.0)
    for name, shape in references.items():
        add_mesh(ax, shape, color_of(name), (0, 0, lift.get(name, 0)))
    ax.set_xlim(-30, 30)
    ax.set_ylim(-30, 30)
    ax.set_zlim(-10, 50)
    ax.set_box_aspect((1, 1, 1))
    ax.view_init(elev=24, azim=-58)
    ax.set_axis_off()
    ax.set_title('VoiceStick C3 v1 enclosure, exploded (+X = USB end)')
    fig.savefig(path, dpi=110, bbox_inches='tight')
    plt.close(fig)


def draw_section(ax, shapes, normal, position, axes):
    """Fill the cut outline of each shape on plane normal . p = position."""
    for name, shape in shapes.items():
        for wire in shape.slice(normal, position):
            points = wire.discretize(Deflection=0.02)
            xy = [(getattr(p, axes[0]), getattr(p, axes[1])) for p in points]
            ax.add_patch(Polygon(xy, closed=True, facecolor=color_of(name), edgecolor='black',
                                 linewidth=0.3, alpha=0.85))
    ax.set_aspect('equal')
    ax.grid(True, linewidth=0.3)


def sections(parts, references, path):
    shapes = {**parts, **references}
    port_y = be.IF['microphone']['port_xy'][1]
    sw2 = next(k for k in be.IF['side_keys']['keys'] if k['ref'] == 'SW2')
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(13, 10))
    draw_section(top, shapes, be.V(0, 1, 0), port_y, ('x', 'z'))
    top.set_xlim(-28, 28)
    top.set_ylim(-0.5, 21)
    top.set_title(f'Section Y = {port_y} (microphone duct, foam, screw H2): X / Z in mm')
    draw_section(bottom, shapes, be.V(1, 0, 0), sw2['x'], ('y', 'z'))
    bottom.set_xlim(-18, 18)
    bottom.set_ylim(-0.5, 21)
    bottom.set_title(f"Section X = {sw2['x']} (SW2 side key, main key): Y / Z in mm")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)


def main():
    names = ['LowerShell', 'UpperShell', 'MainCap'] + [f"SideCap_{k['ref']}" for k in be.IF['side_keys']['keys']]
    parts = {name: load_part(name) for name in names}
    components = be.build_components()
    housing, stem = be.build_main_switch()
    references = {
        'PCB': be.build_pcb(),
        'Parts': be.fuse([b for ref, b in components.items() if not ref.endswith('_leads')]),
        'SW1': be.fuse([housing, stem]),
        'Battery': be.build_measured_battery(),
        'Foam': be.build_foam(),
    }
    exploded_view(parts, references, be.OUT / 'preview-exploded.png')
    sections(parts, references, be.OUT / 'preview-sections.png')
    print(f'previews written to {be.OUT}')


if __name__ == '__main__':
    main()
