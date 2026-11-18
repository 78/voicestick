# Build a VoiceStick C3 v1

English · [简体中文](README_ZH.md)

VoiceStick C3 v1 is the first public hardware design for the Bluetooth talk button. It pairs an ESP32-C3 and a PDM microphone with a main key, three side keys and an RGB status LED on a 50 × 30 mm board.

Each board has its own directory under `hardware/`, named after the matching firmware board. The current C3 design is [voicestick_c3](voicestick_c3/README.md), hardware revision v1.

## v1 release files

For fabrication and printing, extract `voicestick-c3-v1-build-files.zip`. For a populated board in KiCad’s 3D viewer or a full board STEP export, extract `voicestick-c3-v1-component-models.zip` into `hardware/voicestick_c3/` in the source checkout. This places seven STEP files in `kicad/3dmodels/`. [VoiceStick C3 v1 release](https://github.com/78/voicestick/releases/tag/hw-voicestick-c3-v1)

1. **Order the PCB.** In the build archive, open `fab/`. Use the four-layer, 1.0 mm stack-up in the [fabrication guide](voicestick_c3/fab/README.md). Upload the Gerber ZIP, BOM and placement file. Leave C25 and C26 unpopulated and check component rotations in the assembly preview.
2. **Print the enclosure.** Start with the tolerance coupon. The STL parts (`enclosure/out/stl/`) are oriented for printing. The individual STEP solids (`enclosure/out/step/`) can be edited in CAD or opened in a STEP-capable slicer. Set the print face using the [printing and assembly guide](voicestick_c3/enclosure/README.md). The complete assembly STEP (`enclosure/out/voicestick-c3-v1-assembly.step`) shows the PCB, enclosure and battery in position.
3. **Power up and flash.** Start the USB power checks with the battery disconnected. Use a protected, two-wire 602025 250 mAh cell. On the JST SH 1.0 mm connector J2, pin 1 is battery positive and pin 2 is ground. Activate ESP-IDF release-v6.1, then run the following from the repository root, substituting your serial port. The release binaries target StickS3. Build the C3 image with `voicestick_c3`.

```sh
cd firmware
idf.py -B build-c3 -DBOARD=voicestick_c3 build
idf.py -B build-c3 -p /dev/cu.usbmodemXXXX flash monitor
```

**Verification.** Review the [ERC](voicestick_c3/kicad/erc.rpt), [DRC](voicestick_c3/kicad/drc.rpt) and [enclosure clearance report](voicestick_c3/enclosure/out/enclosure-report.json). Microphone noise, antenna matching with the case fitted and deep-sleep current still need measurements on assembled hardware. See the [first-board checks](voicestick_c3/README.md#first-board-checks).

For modifications, start with the [design notes and GPIO map](voicestick_c3/README.md), schematic PDF (`kicad/voicestick-c3-v1-schematic.pdf`) or [KiCad project](voicestick_c3/kicad/voicestick-c3-v1.kicad_pro).
