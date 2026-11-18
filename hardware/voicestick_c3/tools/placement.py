"""Footprint placement in mechanical coordinates.

X points to the USB end, +Y is the right-hand side in the holding view (see
assembly-interface.json). Rotation is the KiCad footprint orientation in
degrees. Mechanically constrained parts (keys, USB, mic, antenna, LEDs,
battery connector, holes) are read from assembly-interface.json so the
enclosure and the PCB cannot disagree; everything else is listed here.
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INTERFACE = json.loads((ROOT / 'assembly-interface.json').read_text())


def mechanical():
    i = INTERFACE
    out = {
        'SW1': (*i['main_key']['center_xy'], i['main_key']['kicad_rotation']),
        'J1': (*i['usb']['origin_xy'], i['usb']['kicad_rotation']),
        'U5': (*i['microphone']['port_xy'], i['microphone']['kicad_rotation']),
        'AE1': (*i['antenna']['center_xy'], i['antenna']['kicad_rotation']),
        'J2': (*i['battery']['connector_xy'], i['battery']['connector_kicad_rotation']),
    }
    y0 = i['side_keys']['footprint_origin_abs_y']
    for key in i['side_keys']['keys']:
        out[key['ref']] = (key['x'], key['side'] * y0, key['kicad_rotation'])
    for led in i['light_window']['leds']:
        out[led['ref']] = (*led['xy'], led['kicad_rotation'])
    for hole in i['fasteners']['holes']:
        out[hole['ref']] = (*hole['xy'], 0)
    return out


# Electrical placement. U1 sits with pins 1-8 facing +Y (RF pin 1 towards the
# antenna corner), pins 25-32 facing the USB end and the SPI pins facing -Y.
ELECTRICAL = {
    # MCU and decoupling
    'U1': (6.0, 4.0, 270),
    'C8': (7.48, 8.5, 0),       # VDD3P3, fed straight up from pins 2/3
    'L1': (7.0, 9.98, 270),     # +3V3 -> VDD3P3, pad 2 towards C8
    'C9': (10.15, 6.95, 0),     # VDDA pins 31/32
    'C10': (10.15, 5.9, 0),
    'C11': (1.0, 7.6, 90),     # VDD3P3_RTC pin 11
    'C12': (3.0, 0.3, 0),       # VDD3P3_CPU pin 17
    'C6': (10.0, 12.5, 0),      # +3V3 bulk
    'C7': (10.0, 10.75, 0),
    'R10': (3.4, 8.4, 90),      # EN RC
    'C13': (2.3, 8.4, 90),
    # Chip match, left to right along Y 8.2
    'C18': (9.2, 8.68, 90),
    'L3': (10.7, 8.2, 0),
    'C19': (12.2, 8.68, 90),
    # Antenna match along the feed row Y 13.2, left of the antenna keepout
    'C25': (14.0, 12.72, 270),
    'R20': (15.5, 13.2, 0),
    'C26': (17.0, 12.72, 270),
    # 40 MHz crystal on pins 29/30
    'L2': (10.13, 4.85, 180),
    'Y1': (13.0, 4.2, 270),
    'C14': (12.4, 6.9, 0),
    'C15': (15.4, 3.0, 90),
    # 32.768 kHz crystal above pins 4/5
    'Y2': (4.4, 11.4, 0),
    'C16': (5.65, 13.6, 90),
    'C17': (3.15, 13.6, 90),
    # USB ESD and CC
    'U6': (14.4, 0.25, 0),
    'R1': (13.0, -2.2, 0),
    'R2': (16.2, 4.5, 0),
    'C1': (16.75, -3.6, 270),
    # Charger and power path
    'U2': (12.6, -4.5, 0),
    'R3': (15.4, -3.3, 90),
    'C2': (9.6, -4.8, 90),
    'D1': (11.8, -7.6, 0),
    'Q1': (11.6, -10.6, 0),
    'R5': (9.1, -9.8, 90),
    'R4': (14.2, -10.0, 90),
    # LDO
    'U3': (7.6, -13.2, 0),
    'C3': (4.4, -12.6, 90),
    'C4': (11.3, -13.6, 0),
    # Sensing
    'R6': (0.0, 9.2, 0),
    'R7': (0.0, 10.3, 0),
    'C5': (0.0, 11.4, 0),
    'R8': (15.4, -10.3, 270),
    'R9': (15.4, -12.3, 270),
    # Flash
    'U4': (5.6, -5.0, 90),
    'C20': (2.1, -1.8, 90),
    'C21': (1.0, -1.8, 90),
    'R12': (2.1, -6.5, 90),
    # Switched peripheral rail
    'Q2': (-5.4, 10.4, 0),
    'R13': (-4.8, 7.6, 90),
    'C22': (-20.2, -11.8, 90),
    'R14': (-16.0, 8.75, 0),
    'C23': (-19.5, 8.75, 0),
    'C24': (-22.5, 8.9, 0),
    'R15': (-6.0, 4.0, 180),
    # Keys
    'R16': (-6.0, 6.8, 270),
    'R17': (-12.0, -9.6, 0),
    'R19': (-9.5, 10.0, 0),
    'R18': (-4.6, -9.9, 0),
    # Test points
    'TP1': (-2.2, 12.4, 0),
    'TP2': (-1.2, -4.5, 0),
    'TP3': (-3.4, -4.5, 0),
    'TP4': (-6.4, -8.6, 0),
    'TP5': (-6.4, -6.4, 0),
    'TP6': (-4.4, -7.6, 0),
}


def placement():
    out = dict(mechanical())
    clash = set(out) & set(ELECTRICAL)
    assert not clash, clash
    out.update(ELECTRICAL)
    return out
