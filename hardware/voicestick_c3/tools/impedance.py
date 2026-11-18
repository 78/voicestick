#!/usr/bin/env python3
"""50 ohm feed geometry for the JLC 1.0 mm 4-layer stackup (JLC04101H-3313).

The antenna feed runs on L1 over the solid L2 ground with ground pour beside
it, so it is a grounded coplanar waveguide (CPWG). Formula: Wadell,
"Transmission Line Design Handbook" 3.4.3, conformal mapping; copper
thickness is folded in with the usual effective-width correction.
Cross-check the final width with the JLC impedance calculator before ordering.
"""
import math

# JLC04101H-3313: L1-L2 prepreg 3313, 0.0994 mm, Er 4.1; outer copper 1 oz finished.
H = 0.0994
ER = 4.1
T = 0.035


def ellip_k(k):
    """Complete elliptic integral of the first kind via the AGM."""
    a, b = 1.0, math.sqrt(1.0 - k * k)
    while abs(a - b) > 1e-15:
        a, b = (a + b) / 2.0, math.sqrt(a * b)
    return math.pi / (2.0 * a)


def k_ratio(k):
    return ellip_k(k) / ellip_k(math.sqrt(1.0 - k * k))


def cpwg_z0(w, g, h=H, er=ER, t=T):
    # Effective width growth from finite copper thickness.
    dw = (1.25 * t / math.pi) * (1.0 + math.log(4.0 * math.pi * w / t))
    we, ge = w + dw, max(g - dw, 1e-6)
    k1 = we / (we + 2.0 * ge)
    k3 = math.tanh(math.pi * we / (4.0 * h)) / math.tanh(math.pi * (we + 2.0 * ge) / (4.0 * h))
    ratio = k_ratio(k3) / k_ratio(k1)
    eeff = (1.0 + er * ratio) / (1.0 + ratio)
    z0 = 60.0 * math.pi / math.sqrt(eeff) / (k_ratio(k1) + k_ratio(k3))
    return z0, eeff


def microstrip_z0(w, h=H, er=ER, t=T):
    """Hammerstad-Jensen microstrip, used as a sanity check with a wide gap."""
    we = w + (t / math.pi) * (1.0 + math.log(2.0 * h / t))
    u = we / h
    a = 1 + (1 / 49) * math.log((u ** 4 + (u / 52) ** 2) / (u ** 4 + 0.432)) + (1 / 18.7) * math.log(1 + (u / 18.1) ** 3)
    b = 0.564 * ((er - 0.9) / (er + 3)) ** 0.053
    eeff = (er + 1) / 2 + (er - 1) / 2 * (1 + 10 / u) ** (-a * b)
    f = 6 + (2 * math.pi - 6) * math.exp(-((30.666 / u) ** 0.7528))
    z0 = 60 / math.sqrt(eeff) * math.log(f / u + math.sqrt(1 + (2 / u) ** 2))
    return z0, eeff


def width_for(z_target, g):
    lo, hi = 0.05, 0.6
    for _ in range(60):
        mid = (lo + hi) / 2
        if cpwg_z0(mid, g)[0] > z_target:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


# Chosen geometry, read by build_pcb.py. 0.15 mm sits between the CPWG and the
# plain-microstrip estimates, so pour clearance tolerance moves Z0 by only a few ohms.
# It is also narrower than the 0.25 mm QFN RF pad, so the feed leaves the pin
# without a width step.
RF_GAP = 0.25
RF_WIDTH = 0.15

if __name__ == '__main__':
    print(f'stackup: h={H} mm, Er={ER}, t={T} mm')
    for g in (0.15, 0.20, 0.25, 0.30):
        w = width_for(50.0, g)
        print(f'CPWG gap {g:.2f} mm -> width {w:.3f} mm for 50 ohm')
    ms = microstrip_z0(RF_WIDTH)[0]
    z, e = cpwg_z0(RF_WIDTH, RF_GAP)
    print(f'chosen: width {RF_WIDTH:.3f} mm, gap {RF_GAP:.2f} mm -> CPWG {z:.1f} ohm (eeff {e:.2f}); '
          f'same width as plain microstrip {ms:.1f} ohm')
