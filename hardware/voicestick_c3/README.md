# VoiceStick C3 v1 hardware design

English · [简体中文](README_ZH.md)

This is the first public hardware version of VoiceStick C3. To order and assemble a device, start with the [build guide](../README.md). The firmware board identifier is `voicestick_c3`.

| Directory or file | Contents |
| --- | --- |
| [kicad/](kicad/) | KiCad project, local symbol and footprint libraries, routed board, ERC and DRC reports |
| [fab/](fab/README.md) | Fabrication guide and PCB renders. Generated manufacturing files use this directory in the build archive. |
| [enclosure/](enclosure/README.md) | FreeCAD scripts, fit checks and assembly instructions. The build archive includes the printable parts and assembly STEP. |
| [tools/](tools/) | Electrical definition, placement, routing, exporters and design checks |
| [assembly-interface.json](assembly-interface.json) | Shared board outline, component positions, clearances and enclosure dimensions |

## Circuit and pins

The QFN32 ESP32-C3 uses a GD25Q64E 8 MB SPI flash, a 40 MHz crystal and a 32.768 kHz sleep crystal. The USB Serial/JTAG console frees the UART0 pins for the keys and LED. A 100 mA TP4054 charger, B5819W USB supply path, AO3401A battery path and RT9080 3.3 V regulator power the board. A second AO3401A switches the microphone and RGB LED together. The SD18OB371-041 bottom-port microphone runs at a default PDM clock of 1.536 MHz, decimated by 96 for 16 kHz audio.

| GPIO | Function |
| --- | --- |
| 0, 1 | 32.768 kHz crystal |
| 2 | Active-low microphone and RGB rail enable, 10 kΩ pull-up |
| 3 | Battery ADC, 1 MΩ / 1 MΩ divider |
| 4 | Main key, active-low wake with 100 kΩ pull-up |
| 5 | USB presence, active-high wake, 100 kΩ / 200 kΩ divider |
| 6, 7 | PDM clock through 33 Ω, and PDM data |
| 8 | Previous key, 10 kΩ boot-strap pull-up |
| 9 | Cancel / BOOT key, 10 kΩ pull-up |
| 10 | TP4054 charging status, active low, enable internal pull-up |
| 11–17 | External flash supply and SPI |
| 18, 19 | USB D− and D+ |
| 20 | WS2812B-2020-V6 data |
| 21 | Next key through 1 kΩ |

The GPIO8 pull-up stays on the previous key to avoid back-powering the unpowered LED. GPIO20 floats before deep sleep. After one hour idle on battery, or one minute without a connected host, the main key or USB can wake the board. The low-battery path uses USB-only wake after three readings below 3.5 V.

## Editing and rebuilding

For full populated PCB and assembly STEP exports, unpack the matching component model archive as described under [v1 release files](../README.md#v1-release-files). Circuit edits, routing and electrical checks can be done from the source checkout alone. For an enclosure-only rebuild, extract `fab/voicestick-c3-v1.step` from the build archive to include the populated PCB.

Edit connectivity and component selections in [design.py](tools/design.py), drawing layout in [schematic_layout.py](tools/schematic_layout.py) and board placement in [placement.py](tools/placement.py). The generated KiCad files are available for inspection and fabrication. The electrical definition also drives the BOM and the pin-by-pin netlist check.

The build script uses the macOS application paths for KiCad 10 and FreeCAD 1.1. Install KiCadRoutingTools with its Python virtual environment under `~/tools/KiCadRoutingTools`, or point `KRT` at the checkout. [KiCadRoutingTools](https://github.com/drandyhaas/KiCadRoutingTools)

```sh
cd hardware/voicestick_c3
./make.sh             # Complete PCB and enclosure build
./make.sh board       # Through board DRC and geometry checks
./make.sh enclosure   # Rebuild enclosure from the existing board
```

The build generates the libraries, schematic, BOM and netlist, places the footprints, locks critical copper, routes the other nets and refills the planes. It checks schematic parity and board rules before exporting the manufacturing files and enclosure models. The fixed UUID seeds and clean intermediate directory keep the routing order reproducible. After layout changes, `python3 tools/seed_search.py --sets 0-29 --jobs 5` compares route quality. The current layout uses seed set 8.

## Layout and components

The board measures 50 × 30 × 1.0 mm with four copper layers, internal GND and +3V3 planes, and components on top. RF, USB, clocks, flash, decoupling and the power trunks have locked routing. Signal and light-load branches use 0.45/0.3 mm vias. Supply trunks and ground stitching use 0.6/0.3 mm vias. U1's nine thermal vias are the only vias in pads. The USB traces are short and the case limits antenna clearance, so RF matching is checked with the case fitted.

The main switch is CPG151101D05, and the three side switches are Alps SKRTLBE010. The build archive’s `fab/voicestick-c3-v1-bom.csv` lists manufacturer and LCSC numbers. The custom 3D library includes aligned EasyEDA models or size substitutes for the side switch, microphone, antenna and LEDs. The main switch model is generated from the mechanical interface. Clearance checks use component envelopes.

## First-board checks

ERC, DRC, netlist parity and enclosure geometry are checked by the build. The nine STL meshes are closed. Record physical results separately:

1. Verify the BOM matches and placement rotations in the order preview. Confirm the RF feed impedance with the selected JLC04101H-3313 stack-up.
2. Print the fit coupon and acoustic test box. Check the stem, screw pilots, key travel and microphone seal.
3. Check the supply rails over USB before connecting a battery. Measure crystal offset, PDM clock, microphone format and noise. Tune chip-side matching first, then measure antenna matching and range with the enclosure fitted.
4. On battery power, measure deep-sleep current at J2. The design estimate is 12–20 µA. Check reverse leakage through D1 and that the charging LED goes out with USB removed. Verify that low-battery shutdown wakes only on USB insertion.

The [fabrication guide](fab/README.md) and [enclosure guide](enclosure/README.md) give the test points and assembly details.
