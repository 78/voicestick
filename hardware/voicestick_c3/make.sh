#!/usr/bin/env bash
# VoiceStick C3 v1 build pipeline. Every KiCad file is generated; never edit them by hand.
#
#   ./make.sh            full build: library -> schematic -> PCB -> routing -> checks -> exports
#   ./make.sh enclosure  enclosure only (FreeCAD)
#   ./make.sh board      stop after DRC and board checks (used by tools/seed_search.py)
#
# Board stages in build/ (each file is the input of the next):
#   01_placed      footprints, outline, keepouts, net classes        (build_pcb.py)
#   02_critical    locked RF / USB / crystal / decoupling copper      (route_critical.py)
#   03_planes      GND on In1.Cu, +3V3 on In2.Cu                      (KRT route_planes)
#   04_routed      every remaining net, 0.45/0.3 vias                 (KRT route)
#   05_vias        power vias grown to 0.6/0.3, misfits ripped        (upsize_power_vias.py)
#   05b_power_fix  ripped power gaps re-routed with 0.6/0.3 vias      (KRT route)
#   05c_check      same board, build stops if a power via is still small
#   06_pours       GND on F.Cu/B.Cu, 2.5 GHz stitching + edge fence (0.6/0.3 vias)
#   07_final       plane finalize: pads welded, pours verified        (KRT route)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

KICAD_APP=/Applications/KiCad/KiCad.app
KPY="$KICAD_APP/Contents/Frameworks/Python.framework/Versions/Current/bin/python3"
KCLI="$KICAD_APP/Contents/MacOS/kicad-cli"
FREECAD_PY=/Applications/FreeCAD.app/Contents/Resources/bin/python
KRT="${KRT:-$HOME/tools/KiCadRoutingTools}"
B="$HERE/build"
FAB="$HERE/fab"
PROJECT=voicestick-c3-v1

# KiCad's python prints a harmless wx "traits" assert on start-up.
kpy() { "$KPY" "$@" 2> >(grep -v -e traits -e 'swig/python detected' >&2); }
krt() { (cd "$KRT" && .venv/bin/python "$@"); }
step() { printf '\n== %s\n' "$*"; }
# Stop as soon as a KRT step ships an incomplete net.
complete() {
    grep -E 'Single-ended:|Multi-point:|nothing to route' "$1" | tail -2 || true
    if grep -q 'Ships broken' "$1"; then echo "incomplete routing, see $1" >&2; exit 1; fi
}

# Routing rules shared by every KRT step: standard JLC tier, no silent escalation,
# no via-in-pad, inner layers are planes only.
KRT_RULES=(--track-width 0.15 --clearance 0.127
           --layers F.Cu In1.Cu In2.Cu B.Cu --layer-costs 1 -1 -1 1.5
           --fab-tier standard --escalation off --same-net-pad-clearance 0.1)
# VBUS, VBAT and VSYS trunks are hand-routed in route_critical.py; what KRT
# still connects on them are divider and gate branches, so they route as signals.
POWER=(--power-nets +3V3 VDD3P3 PERIPH_3V3 MIC_VDD VDD_SPI GND
       --power-nets-widths 0.4 0.3 0.3 0.25 0.25 0.3)

enclosure() {
    step "enclosure (FreeCAD): parts, print STLs, interference checks"
    kpy tools/export_envelopes.py "kicad/$PROJECT.kicad_pcb"
    "$FREECAD_PY" enclosure/build_enclosure.py
    "$FREECAD_PY" enclosure/render_preview.py
}

if [[ "${1:-}" == enclosure ]]; then enclosure; exit 0; fi

step "library, schematic, ERC, netlist, BOM"
python3 tools/build_library.py
"$FREECAD_PY" enclosure/build_switch_model.py
python3 tools/build_schematic.py
python3 tools/draw_orientation.py
(cd kicad && "$KCLI" sch erc --severity-all --exit-code-violations -o erc.rpt "$PROJECT.kicad_sch")
(cd kicad && "$KCLI" sch export netlist -o "$PROJECT.net" "$PROJECT.kicad_sch")
(cd kicad && "$KCLI" sch export pdf -o "$PROJECT-schematic.pdf" "$PROJECT.kicad_sch")
python3 tools/check_netlist.py
python3 tools/export_bom.py

step "placement and hand-routed critical copper"
# Stale stage files change the result: NewBoard/LoadBoard pick up an existing
# .kicad_pro next to the board. Every build starts from an empty stage set.
rm -f "$B"/0[1-7]*
kpy tools/build_pcb.py
kpy tools/route_critical.py

step "KRT: inner planes"
krt py_router/route_planes.py "$B/02_critical.kicad_pcb" "$B/03_planes.kicad_pcb" \
    --nets GND +3V3 --plane-layers In1.Cu In2.Cu \
    --via-size 0.45 --via-drill 0.3 --clearance 0.127 --fab-tier standard --escalation off \
    > "$B/03_planes.log" 2>&1

step "KRT: route all nets"
krt py_router/route.py "$B/03_planes.kicad_pcb" "$B/04_routed.kicad_pcb" --nets '*' \
    --via-size 0.45 --via-drill 0.3 "${POWER[@]}" "${KRT_RULES[@]}" > "$B/04_routed.log" 2>&1
complete "$B/04_routed.log"

step "power vias to 0.6/0.3"
kpy tools/upsize_power_vias.py "$B/04_routed.kicad_pcb" "$B/05_vias.kicad_pcb" --rip-failed
if [[ -s "$B/05_vias.ripped" ]]; then
    # '*' so that a net KRT rips out of the way is re-routed too; connected
    # nets are left alone. Any via laid here is power size (also legal for signals).
    krt py_router/route.py "$B/05_vias.kicad_pcb" "$B/05b_power_fix.kicad_pcb" --nets '*' \
        --via-size 0.6 --via-drill 0.3 "${POWER[@]}" "${KRT_RULES[@]}" > "$B/05b_power_fix.log" 2>&1
    complete "$B/05b_power_fix.log"
else
    for ext in kicad_pcb kicad_pro kicad_dru; do cp "$B/05_vias.$ext" "$B/05b_power_fix.$ext"; done
fi
# Fails the build if any power via is still below 0.6/0.3.
kpy tools/upsize_power_vias.py "$B/05b_power_fix.kicad_pcb" "$B/05c_check.kicad_pcb"

step "KRT: outer GND pours, stitching, finalize"
krt py_router/route_planes.py "$B/05c_check.kicad_pcb" "$B/06_pours.kicad_pcb" \
    --nets GND GND GND +3V3 --plane-layers In1.Cu F.Cu B.Cu In2.Cu \
    --via-size 0.6 --via-drill 0.3 --clearance 0.127 --zone-clearance 0.2 \
    --stitch-vias --stitch-max-freq 2500 --stitch-edge-fence \
    --fab-tier standard --escalation off --same-net-pad-clearance 0.1 > "$B/06_pours.log" 2>&1
grep -E 'vias placed' "$B/06_pours.log" || true
krt py_router/route.py "$B/06_pours.kicad_pcb" "$B/07_final.kicad_pcb" --nets '*' \
    --via-size 0.45 --via-drill 0.3 "${POWER[@]}" "${KRT_RULES[@]}" > "$B/07_final.log" 2>&1
complete "$B/07_final.log"

step "install board, DRC with schematic parity, board checks"
kpy tools/finish_board.py "$B/07_final.kicad_pcb"
(cd kicad && "$KCLI" pcb drc --severity-error --schematic-parity --refill-zones \
    --exit-code-violations -o drc.rpt "$PROJECT.kicad_pcb")
echo "DRC: 0 errors (kicad/drc.rpt)"
kpy tools/check_board.py "kicad/$PROJECT.kicad_pcb"
if [[ "${1:-}" == board ]]; then exit 0; fi

step "fabrication exports"
rm -rf "$FAB/gerber" && mkdir -p "$FAB/gerber"
"$KCLI" pcb export gerbers -o "$FAB/gerber/" --subtract-soldermask \
    -l F.Cu,In1.Cu,In2.Cu,B.Cu,F.Mask,B.Mask,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,Edge.Cuts \
    "kicad/$PROJECT.kicad_pcb" > /dev/null
# No drill maps: their legend widens the extents JLC reads as the board size.
"$KCLI" pcb export drill -o "$FAB/gerber/" --format excellon --excellon-separate-th \
    "kicad/$PROJECT.kicad_pcb" > /dev/null
(cd "$FAB/gerber" && rm -f ../"$PROJECT-gerber.zip" && zip -X -q ../"$PROJECT-gerber.zip" ./*)
"$KCLI" pcb export pos --format csv --units mm --side front --exclude-dnp \
    -o "$B/pos.csv" "kicad/$PROJECT.kicad_pcb" > /dev/null
python3 tools/export_cpl.py "$B/pos.csv" "$FAB/$PROJECT-cpl.csv"
cp bom.csv "$FAB/$PROJECT-bom.csv"
"$KCLI" pcb export step --subst-models --no-dnp -o "$FAB/$PROJECT.step" \
    "kicad/$PROJECT.kicad_pcb" > /dev/null 2>&1 || echo "warning: STEP export failed"
"$KCLI" pcb render --side top --width 2000 --height 1300 --quality high \
    -o "$FAB/$PROJECT-top.png" "kicad/$PROJECT.kicad_pcb" > /dev/null
"$KCLI" pcb render --side bottom --width 2000 --height 1300 --quality high \
    -o "$FAB/$PROJECT-bottom.png" "kicad/$PROJECT.kicad_pcb" > /dev/null
ls "$FAB"

enclosure
