"""VoiceStick C3 v1 electrical design: the single source of truth.

Every part lists its symbol, footprint, value and a pin-number -> net map.
The schematic, BOM and PCB are generated from this file; the PCB netlist is
then re-read from the KiCad schematic export, so the two cannot drift apart.
"""
import os
from collections import OrderedDict

LIB = 'VoiceStickC3V1'
R0402 = 'Resistor_SMD:R_0402_1005Metric'
C0402 = 'Capacitor_SMD:C_0402_1005Metric'
C0603 = 'Capacitor_SMD:C_0603_1608Metric'
L0402 = 'Inductor_SMD:L_0402_1005Metric'

PARTS = OrderedDict()

# (footprint, value) -> (LCSC, MPN) for passives. All are JLC basic parts except
# the RF ones (0402 inductors, 1.5 pF), which have no basic-library equivalent.
PASSIVES = {
    (R0402, '0R'): ('C17168', '0402WGF0000TCE'),
    (R0402, '33R'): ('C25105', '0402WGF330JTCE'),
    (R0402, '100R'): ('C25076', '0402WGF1000TCE'),
    (R0402, '1k'): ('C11702', '0402WGF1001TCE'),
    (R0402, '5.1k'): ('C25905', '0402WGF5101TCE'),
    (R0402, '10k'): ('C25744', '0402WGF1002TCE'),
    (R0402, '100k'): ('C25741', '0402WGF1003TCE'),
    (R0402, '200k'): ('C25764', '0402WGF2003TCE'),
    (R0402, '1M'): ('C26083', '0402WGF1004TCE'),
    (C0402, '1.5pF'): ('C1552', '0402CG1R5C500NT'),
    (C0402, '18pF'): ('C1549', '0402CG180J500NT'),
    (C0402, '22pF'): ('C1555', '0402CG220J500NT'),
    (C0402, '100nF'): ('C1525', 'CL05B104KO5NNNC'),
    (C0402, '1uF'): ('C52923', 'CL05A105KA5NQNC'),
    (C0402, '4.7uF'): ('C23733', 'CL05A475MP5NRNC'),
    (C0603, '4.7uF 16V'): ('C19666', 'CL10A475KO8NNNC'),
    (C0603, '10uF 10V'): ('C19702', 'CL10A106KP8NNNC'),
    (L0402, '2nH'): ('C18216', 'Murata LQG15HS2N0S02D'),
    (L0402, '2.4nH'): ('C97997', 'Murata LQG15HS2N4S02D'),
    (L0402, '24nH'): ('C74301', 'Sunlord SDCL1005C24NJTDF'),
}

# KiCad sorts footprints by UUID when it saves a board, and KRT routes in file
# order, so random UUIDs give a different (sometimes failing) route every build.
# Each pcbnew stage seeds the generator with its own value: reproducible files,
# no UUID shared between stages. KIID_SEED_SET picks another set of seeds, i.e.
# another KRT routing order; tools/seed_search.py scans sets and the chosen one
# is frozen here.
KIID_SEED_SET = int(os.environ.get('KIID_SEED_SET', 8))
KIID_SEEDS = {stage: KIID_SEED_SET * 10 + offset
              for stage, offset in (('build_pcb', 1), ('route_critical', 2), ('upsize_power_vias', 3))}


def part(ref, lib_id, value, footprint, pins, block, lcsc='', mpn='', note='', dnp=False):
    assert ref not in PARTS, ref
    if not lcsc and (footprint, value) in PASSIVES:
        lcsc, mpn = PASSIVES[(footprint, value)]
    PARTS[ref] = dict(ref=ref, lib_id=lib_id, value=value, footprint=footprint,
                      pins={str(k): v for k, v in pins.items()}, block=block,
                      lcsc=lcsc, mpn=mpn or value, note=note, dnp=dnp)


def res(ref, value, a, b, block, note='', dnp=False):
    part(ref, 'Device:R_Small', value, R0402, {1: a, 2: b}, block, note=note, dnp=dnp)


def cap(ref, value, a, b, block, footprint=C0402, note='', dnp=False):
    part(ref, 'Device:C_Small', value, footprint, {1: a, 2: b}, block, note=note, dnp=dnp)


# ---------------------------------------------------------------- USB input
part('J1', 'Connector:USB_C_Receptacle_USB2.0_16P', 'TYPE-C-31-M-12',
     'Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12',
     {'A1': 'GND', 'B1': 'GND', 'A12': 'GND', 'B12': 'GND', 'SH': 'GND',
      'A4': 'VBUS', 'B4': 'VBUS', 'A9': 'VBUS', 'B9': 'VBUS',
      'A5': 'CC1', 'B5': 'CC2', 'A6': 'USB_DP', 'B6': 'USB_DP', 'A7': 'USB_DN', 'B7': 'USB_DN',
      'A8': None, 'B8': None}, 'usb', lcsc='C165948', mpn='HRO TYPE-C-31-M-12')
res('R1', '5.1k', 'CC1', 'GND', 'usb')
res('R2', '5.1k', 'CC2', 'GND', 'usb')
part('U6', 'Power_Protection:USBLC6-2SC6', 'USBLC6-2SC6', 'Package_TO_SOT_SMD:SOT-23-6',
     {1: 'USB_DP', 6: 'USB_DP', 3: 'USB_DN', 4: 'USB_DN', 2: 'GND', 5: 'VBUS'}, 'usb', lcsc='C7519')
cap('C1', '4.7uF 16V', 'VBUS', 'GND', 'usb', C0603)

# ------------------------------------------------- charger and power path
part('U2', f'{LIB}:TP4054', 'TP4054', 'Package_TO_SOT_SMD:SOT-23-5',
     {4: 'VBUS', 5: 'PROG', 2: 'GND', 3: 'VBAT', 1: 'CHRG'}, 'charge',
     lcsc='C32574', mpn='TOP POWER TP4054-42-SOT25R', note='Charge current 1000 V / 10k = 100 mA (0.4C)')
res('R3', '10k', 'PROG', 'GND', 'charge')
cap('C2', '4.7uF 16V', 'VBAT', 'GND', 'charge', C0603)
part('J2', 'Connector_Generic:Conn_01x02', 'BAT 602025', 'Connector_JST:JST_SH_SM02B-SRSS-TB_1x02-1MP_P1.00mm_Horizontal',
     {1: 'VBAT', 2: 'GND', 'MP': None}, 'charge', lcsc='C160402', mpn='JST SM02B-SRSS-TB(LF)(SN)',
     note='Pin 1 = battery +')
# Charge LED: 3V3 -> R -> LED -> open-drain CHRG; GPIO10 reads the same node.
# Once charged, CHRG sinks a weak ~20 uA: the LED glows faintly and GPIO10
# needs its internal pull-up to read high.
res('R4', '1k', '+3V3', 'CHG_LED_A', 'charge')
part('D3', 'Device:LED_Small', 'RED', f'{LIB}:LED_LiteOn_LTST-S270',
     {1: 'CHRG', 2: 'CHG_LED_A'}, 'charge', lcsc='C125114', mpn='Lite-On LTST-S270KRKT',
     note='Side-looking, shines through the USB end wall')
# VBUS -> Schottky -> VSYS; battery -> PMOS (source on VSYS, drain on VBAT) -> VSYS.
part('D1', 'Device:D_Schottky_Small', 'B5819W', 'Diode_SMD:D_SOD-123',
     {1: 'VSYS', 2: 'VBUS'}, 'path', lcsc='C8598', mpn='B5819W SL')
part('Q1', 'Transistor_FET:Q_PMOS_GSD', 'AO3401A', 'Package_TO_SOT_SMD:SOT-23',
     {1: 'VBUS', 2: 'VSYS', 3: 'VBAT'}, 'path', lcsc='C15127',
     note='Off while VBUS present; body diode VBAT->VSYS')
res('R5', '100k', 'VBUS', 'GND', 'path', note='Q1 gate pull-down')
cap('C3', '4.7uF 16V', 'VSYS', 'GND', 'path', C0603)

# ------------------------------------------------------------------- LDO
part('U3', f'{LIB}:RT9080-33', 'RT9080-33GJ5', 'Package_TO_SOT_SMD:SOT-23-5',
     {1: 'VSYS', 3: 'VSYS', 2: 'GND', 5: '+3V3', 4: None}, 'ldo', lcsc='C841192',
     mpn='Richtek RT9080-33GJ5 (TSOT-23-5)')
cap('C4', '10uF 10V', '+3V3', 'GND', 'ldo', C0603)

# ---------------------------------------------------------------- sensing
res('R6', '1M', 'VBAT', 'BAT_ADC', 'sense')
res('R7', '1M', 'BAT_ADC', 'GND', 'sense')
cap('C5', '100nF', 'BAT_ADC', 'GND', 'sense')
res('R8', '100k', 'VBUS', 'VBUS_SENSE', 'sense')
res('R9', '200k', 'VBUS_SENSE', 'GND', 'sense', note='5 V -> 3.33 V')

# -------------------------------------------------------------------- MCU
part('U1', 'MCU_Espressif:ESP32-C3', 'ESP32-C3', 'Package_DFN_QFN:QFN-32-1EP_5x5mm_P0.5mm_EP3.7x3.7mm',
     {1: 'LNA_IN', 2: 'VDD3P3', 3: 'VDD3P3', 4: 'X32_P', 5: 'X32_N', 6: 'PERIPH_EN', 7: 'CHIP_EN',
      8: 'BAT_ADC', 9: 'KEY_MAIN', 10: 'VBUS_SENSE', 11: '+3V3', 12: 'PDM_CLK_MCU', 13: 'PDM_DAT',
      14: 'KEY_PREV', 15: 'KEY_CANCEL', 16: 'CHRG', 17: '+3V3', 18: 'VDD_SPI', 19: 'SPI_HD',
      20: 'SPI_WP', 21: 'SPI_CS', 22: 'SPI_CLK', 23: 'SPI_D', 24: 'SPI_Q', 25: 'USB_DN', 26: 'USB_DP',
      27: 'LED_DIN', 28: 'KEY_NEXT_MCU', 29: 'XTAL_N', 30: 'XTAL_P', 31: '+3V3', 32: '+3V3', 33: 'GND'},
     'mcu', lcsc='C2838500', mpn='ESP32-C3 (QFN32, no flash)')
cap('C6', '10uF 10V', '+3V3', 'GND', 'mcu', C0603)
cap('C7', '100nF', '+3V3', 'GND', 'mcu')
part('L1', 'Device:L_Small', '2nH', L0402, {1: '+3V3', 2: 'VDD3P3'}, 'mcu', note='VDD3P3 RF filter')
cap('C8', '100nF', 'VDD3P3', 'GND', 'mcu')
cap('C9', '1uF', '+3V3', 'GND', 'mcu', note='VDDA')
cap('C10', '100nF', '+3V3', 'GND', 'mcu', note='VDDA')
cap('C11', '100nF', '+3V3', 'GND', 'mcu', note='VDD3P3_RTC')
cap('C12', '100nF', '+3V3', 'GND', 'mcu', note='VDD3P3_CPU')
res('R10', '10k', '+3V3', 'CHIP_EN', 'mcu')
cap('C13', '1uF', 'CHIP_EN', 'GND', 'mcu', note='EN power-up delay')

# ----------------------------------------------------------------- clocks
part('Y1', 'Device:Crystal_GND24', '40MHz', 'Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm',
     {1: 'X40_P', 3: 'XTAL_N', 2: 'GND', 4: 'GND'}, 'clock', lcsc='C9010',
     mpn='Yangxing X322540MPB4SI (CL=15pF +-10ppm)')
cap('C14', '22pF', 'X40_P', 'GND', 'clock', note='Tune to crystal CL')
cap('C15', '22pF', 'XTAL_N', 'GND', 'clock', note='Tune to crystal CL')
part('L2', 'Device:L_Small', '24nH', L0402, {1: 'X40_P', 2: 'XTAL_P'}, 'clock',
     note='Espressif reference series element on XTAL_P')
part('Y2', 'Device:Crystal', '32.768kHz', 'Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm',
     {1: 'X32_P', 2: 'X32_N'}, 'clock', lcsc='C32346', mpn='32.768 kHz 3215 CL=12.5pF')
cap('C16', '18pF', 'X32_P', 'GND', 'clock', note='Tune to crystal CL')
cap('C17', '18pF', 'X32_N', 'GND', 'clock', note='Tune to crystal CL')

# --------------------------------------------------------------------- RF
# Chip match right at pin 1: Espressif's CLC (C 1.2-1.8 pF, L 2.0-3.0 nH,
# C 1.2-1.8 pF), mid values to start; trim for S11 near 35 ohm on the bench.
cap('C18', '1.5pF', 'LNA_IN', 'GND', 'rf', note='Chip match shunt, chip side')
part('L3', 'Device:L_Small', '2.4nH', L0402, {1: 'LNA_IN', 2: 'ANT_FEED'}, 'rf', note='Chip match series')
cap('C19', '1.5pF', 'ANT_FEED', 'GND', 'rf', note='Chip match shunt, line side')
# Antenna match at the keepout edge, tuned with a VNA once the case is closed.
cap('C25', 'DNP', 'ANT_FEED', 'GND', 'rf', note='Antenna match shunt, tune with VNA', dnp=True)
res('R20', '0R', 'ANT_FEED', 'ANT', 'rf', note='Antenna match series element')
cap('C26', 'DNP', 'ANT', 'GND', 'rf', note='Antenna match shunt, tune with VNA', dnp=True)
part('AE1', 'Device:Antenna_Chip', 'RFANT3216120A5T', f'{LIB}:ChipAntenna_3216',
     {1: 'ANT', 2: None}, 'rf', lcsc='C127629', mpn='Walsin RFANT3216120A5T')

# ------------------------------------------------------------------ flash
part('U4', 'Memory_Flash:W25Q32JVSS', 'GD25Q64ESIGR', 'Package_SO:SOIC-8_5.3x5.3mm_P1.27mm',
     {1: 'SPI_CS', 2: 'SPI_Q', 3: 'SPI_WP', 4: 'GND', 5: 'SPI_D', 6: 'SPI_CLK', 7: 'SPI_HD', 8: 'VDD_SPI'},
     'flash', lcsc='C2831359', mpn='GigaDevice GD25Q64ESIGR (8 MB, 208 mil)')
cap('C20', '100nF', 'VDD_SPI', 'GND', 'flash')
cap('C21', '1uF', 'VDD_SPI', 'GND', 'flash')
res('R12', '10k', 'VDD_SPI', 'SPI_CS', 'flash')

# ------------------------------------------------- switched peripheral rail
part('Q2', 'Transistor_FET:Q_PMOS_GSD', 'AO3401A', 'Package_TO_SOT_SMD:SOT-23',
     {1: 'PERIPH_EN', 2: '+3V3', 3: 'PERIPH_3V3'}, 'periph', lcsc='C15127',
     note='GPIO2 low = LED and mic powered')
res('R13', '10k', '+3V3', 'PERIPH_EN', 'periph', note='Off by default; GPIO2 strapping high')
part('D2', 'LED:WS2812B-2020', 'WS2812B-2020', 'LED_SMD:LED_WS2812B-2020_PLCC4_2.0x2.0mm',
     {4: 'PERIPH_3V3', 2: 'GND', 3: 'LED_DIN', 1: None}, 'periph', lcsc='C52917434',
     mpn='Worldsemi WS2812B-2020-V6')
cap('C22', '100nF', 'PERIPH_3V3', 'GND', 'periph')
part('U5', f'{LIB}:SD18OB371-041', 'SD18OB371-041', f'{LIB}:Goertek_SD18OB371-041',
     {5: 'MIC_VDD', 4: 'PDM_CLK', 2: 'GND', 1: 'PDM_DAT', 3: 'GND'}, 'periph', lcsc='C3040249',
     note='L/R low = left slot')
res('R14', '100R', 'PERIPH_3V3', 'MIC_VDD', 'periph', note='Mic supply RC filter')
cap('C23', '4.7uF', 'MIC_VDD', 'GND', 'periph')
cap('C24', '100nF', 'MIC_VDD', 'GND', 'periph')
res('R15', '33R', 'PDM_CLK_MCU', 'PDM_CLK', 'periph', note='PDM clock edge damping, at MCU')

# ------------------------------------------------------------------- keys
part('SW1', 'Switch:SW_Push', 'MAIN', f'{LIB}:CPG151101D05', {1: 'KEY_MAIN', 2: 'GND'}, 'keys',
     lcsc='C49234236', mpn='Kailh CPG151101D05', note='Through-hole: JLC THT service or hand solder')
res('R16', '100k', '+3V3', 'KEY_MAIN', 'keys', note='Deep-sleep wake pull-up')
part('SW2', 'Switch:SW_Push', 'PREV', f'{LIB}:Alps_SKRTLBE010', {1: 'KEY_PREV', 2: 'GND'}, 'keys',
     lcsc='C127481', mpn='Alps SKRTLBE010')
res('R17', '10k', '+3V3', 'KEY_PREV', 'keys', note='GPIO8 strapping high')
part('SW3', 'Switch:SW_Push', 'NEXT', f'{LIB}:Alps_SKRTLBE010', {1: 'KEY_NEXT', 2: 'GND'}, 'keys',
     lcsc='C127481', mpn='Alps SKRTLBE010')
res('R18', '1k', 'KEY_NEXT_MCU', 'KEY_NEXT', 'keys', note='GPIO21 prints ROM log at boot')
part('SW4', 'Switch:SW_Push', 'CANCEL/BOOT', f'{LIB}:Alps_SKRTLBE010', {1: 'KEY_CANCEL', 2: 'GND'}, 'keys',
     lcsc='C127481', mpn='Alps SKRTLBE010')
res('R19', '10k', '+3V3', 'KEY_CANCEL', 'keys', note='GPIO9 boot strapping')

# ------------------------------------------------------ test and mounting
for ref, net in (('TP1', 'CHIP_EN'), ('TP2', 'KEY_NEXT_MCU'), ('TP3', 'LED_DIN'),
                 ('TP4', 'GND'), ('TP5', '+3V3'), ('TP6', 'VBAT')):
    part(ref, 'Connector:TestPoint', net, 'TestPoint:TestPoint_Pad_D1.0mm', {1: net}, 'test',
         note='Not populated by assembly')
for ref in ('H2', 'H3'):
    part(ref, 'Mechanical:MountingHole', 'M2', 'MountingHole:MountingHole_2.2mm_M2', {}, 'mech')

# Nets that need a PWR_FLAG because only passive pins drive them.
POWER_FLAGS = ['VBUS', 'VSYS', 'GND', 'VDD3P3', 'PERIPH_3V3', 'MIC_VDD']

# Track widths used by the PCB stages (mm).
POWER_NETS = {'VBUS': 0.5, 'VBAT': 0.5, 'VSYS': 0.5, '+3V3': 0.4, 'VDD3P3': 0.5,
              'PERIPH_3V3': 0.3, 'MIC_VDD': 0.25, 'VDD_SPI': 0.25}
# Routed (not plane) nets that carry more than ~25 mA: charge current, system
# load, radio TX peaks. Every via on them is 0.6/0.3 mm. +3V3 is the In2.Cu
# plane: its entry vias (LDO output, MCU pins 31/32, the VDD3P3 filter) are
# placed at 0.6/0.3 by route_critical.py, while the per-pad taps of pull-ups
# and decoupling caps use the 0.45/0.3 mm signal via, as do the small rails
# (LED + mic, mic filter, flash). A 0.3 mm via still carries well over 1 A.
HIGH_CURRENT_NETS = ('VBUS', 'VBAT', 'VSYS', 'VDD3P3')

GPIO_MAP = [
    ('GPIO0/1', 'X32_P / X32_N', '32.768 kHz crystal'),
    ('GPIO2', 'PERIPH_EN', 'Peripheral rail enable, low = on; 10k pull-up (strapping)'),
    ('GPIO3', 'BAT_ADC', 'Battery voltage, 1M/1M divider (ADC1)'),
    ('GPIO4', 'KEY_MAIN', 'Main key, deep-sleep wake on low, 100k pull-up'),
    ('GPIO5', 'VBUS_SENSE', 'USB present, 100k/200k divider, deep-sleep wake on high'),
    ('GPIO6', 'PDM_CLK_MCU', 'PDM clock, 33R series'),
    ('GPIO7', 'PDM_DAT', 'PDM data'),
    ('GPIO8', 'KEY_PREV', 'Page previous, 10k pull-up (strapping high)'),
    ('GPIO9', 'KEY_CANCEL', 'Cancel / BOOT, 10k pull-up'),
    ('GPIO10', 'CHRG', 'Charge status (low = charging), shared with charge LED; enable internal pull-up'),
    ('GPIO11-17', 'VDD_SPI, SPI_*', 'External flash'),
    ('GPIO18/19', 'USB_DN / USB_DP', 'USB Serial/JTAG console'),
    ('GPIO20', 'LED_DIN', 'WS2812B data (U0RXD released)'),
    ('GPIO21', 'KEY_NEXT_MCU', 'Page next via 1k (U0TXD released)'),
]


def nets():
    result = {}
    for p in PARTS.values():
        for pin, net in p['pins'].items():
            if net:
                result.setdefault(net, []).append((p['ref'], pin))
    return result


if __name__ == '__main__':
    for net, nodes in sorted(nets().items()):
        print(f'{net:14s} {len(nodes):2d}  ' + ' '.join(f'{r}.{p}' for r, p in nodes))
    singles = [n for n, nodes in nets().items() if len(nodes) < 2]
    assert not singles, singles
