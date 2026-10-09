#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Generate the SoundFont and SFZ filter tables for src/filter.cpp.

usage: filter_tables.py <main.dol>

Read a locally supplied Wii Music main.dol (the US release's sys/main.dol); the executable is not distributed with
revosf2. Extract the five biquad preset tables (from 0x804eaa90).

A track's low-pass (0xd8) runs on AX's one-pole filter, y = a0 x + b0 y1 in Q15. NW4R SND turns the argument into
a scale, 1 + (argument - 64) / 64 clamped to 0..1, and the scale into a cutoff of 32000 * 2^(10 (scale - 1)) Hz,
computed in double precision and truncated (Util::CalcLpfFreq). A cutoff of 16 kHz or more switches the filter
off. Only arguments 0 to 57 have a filter. AXGetLpfCoefs computes the coefficients in single precision with
the C library's cosine and square root: c = 2 - cos(2 pi f / 32000), b0 = 32768 (c - sqrt(c^2 - 1)), a0 = 32767
- b0.

The biquad presets (0xb4 type, 0xb5 value, builds from June 2008) are Robert Bristow-Johnson cookbook filters in Q14:
low/high-pass at about -0.5 dB gain and band-pass at 0 dB peak gain. Each coefficient set reduces to a cutoff or centre
frequency, Q and gain.

SFZ: sfizz's lpf_1p has pole exp(-2 pi f / rate); lpf_2p, hpf_2p and bpf_2p use cookbook filters with Q expressed in
dB. Match the game's pole and coefficients at 32000 Hz.

SoundFont: FluidSynth supplies one cookbook low-pass (fluid_iir_filter.c), with Q = 10^((resonance / 10 -
3.01) / 20). It matches the biquad low-pass. Approximate the one-pole by fitting cutoff at 44100 Hz against the game's response at 32000 Hz.
Minimize RMS dB error relative to 20 Hz over 400 log-spaced frequencies from 20 Hz to 14 kHz. Floor both
responses at -30 dB.

Requires Python 3 and numpy.
"""
import argparse
import math
import struct
import sys
from pathlib import Path

try:
    import numpy as np
except ImportError:
    sys.exit("filter_tables.py requires numpy (pip install numpy)")

GAME_RATE = 32000.0  # Hz, AX's output rate
OUT_RATE = 44100.0  # Hz, the rate FluidSynth usually plays at
OFF = 16000  # Hz: AX's filter is off at this cutoff and above
FREQS = np.geomspace(20.0, 14000.0, 400)
FLOOR_DB = -30.0
CUTOFFS = np.arange(1500, 13501, 10)  # the SoundFont cutoffs tried, in absolute cents
SFZ_LOW_PASS_BASE = 20000.0  # Hz: reference cutoff for the SFZ low-pass table (filter open)
SFZ_BIQUAD_BASE = 1000.0  # Hz: reference cutoff for the SFZ biquad table
BIQUADS = {  # type: (address in Wii Music's main.dol, sets, name)
    1: (0x804EAA90, 112, "low-pass"),
    2: (0x804EAEF0, 97, "high-pass"),
    3: (0x804EB2BC, 122, "band-pass 512"),
    4: (0x804EB780, 93, "band-pass 1024"),
    5: (0x804EBB24, 93, "band-pass 2048"),
}
f32 = np.float32


def dol_sections(path):
    """Return a DOL's sections as (address, data) pairs: seven text and eleven data sections."""
    d = path.read_bytes()
    if len(d) < 0x100:
        sys.exit(f"{path} is too short for a DOL")
    offsets = struct.unpack(">18I", d[0x00:0x48])
    addresses = struct.unpack(">18I", d[0x48:0x90])
    sizes = struct.unpack(">18I", d[0x90:0xD8])
    return [(a, d[o:o + s]) for o, a, s in zip(offsets, addresses, sizes) if s]


def read(dol, fmt, address):
    """Unpack `fmt` from the DOL's memory at `address`."""
    for start, data in dol:
        if start <= address and address + struct.calcsize(fmt) <= start + len(data):
            return struct.unpack_from(fmt, data, address - start)
    sys.exit(f"0x{address:08X} isn't in the DOL's sections")


def decode_biquad(kind, coefs):
    """Decode a game biquad's cutoff/centre (Hz at the voice rate), Q and gain (dB) from Q14 coefficients in y = b0 x
    + b1 x1 + b2 x2 + a1 y1 + a2 y2. Measure gain at cookbook unity: DC for low-pass, Nyquist for high-pass,
    centre for band-pass."""
    b0, b1, b2, a1, a2 = (v / 16384.0 for v in coefs)
    alpha = (1.0 + a2) / (1.0 - a2)  # a2 = -(1 - alpha) / (1 + alpha)
    cos_w0 = a1 * (1.0 + alpha) / 2.0  # a1 = 2 cos w0 / (1 + alpha)
    w0 = math.acos(max(-1.0, min(1.0, cos_w0)))
    q = math.sin(w0) / (2.0 * alpha)
    if kind == 1:
        z = 1.0
    elif kind == 2:
        z = -1.0
    else:
        z = complex(math.cos(w0), -math.sin(w0))
    gain = abs((b0 + b1 * z + b2 * z * z) / (1.0 - a1 * z - a2 * z * z))
    return w0 * GAME_RATE / (2.0 * math.pi), q, 20.0 * math.log10(gain)


def cents(ratio):
    return int(round(1200.0 * math.log2(ratio)))


def cutoff_hz(argument):
    """The cutoff NW4R SND gives AX for a 0xd8 argument (SeqTrack::UpdateChannelParam, Channel::Update and
    Util::CalcLpfFreq)."""
    scale = min(max(f32(1.0) + f32(argument - 64) / f32(64.0), f32(0.0)), f32(1.0))
    return int(32000.0 * 2.0 ** (10.0 * (float(scale) - 1.0)))


def one_pole(freq):
    """AXGetLpfCoefs: a0 and b0 in Q15, for a cutoff in Hz."""
    angle = f32(f32(6.2831855) * f32(freq)) / f32(GAME_RATE)
    c = f32(2.0) - f32(math.cos(float(angle)))
    root = f32(math.sqrt(float(c * c - f32(1.0))))
    b0 = int(np.int32(f32(32768.0) * -(root - c))) & 0xffff
    return 0x7fff - b0, b0


def response_db(b, a, rate, freqs=FREQS):
    """Compute the response of y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2 at `rate`. Return dB relative to the
    first frequency, clamped to the floor."""
    z = np.exp(-2j * np.pi * freqs / rate)
    h = (b[0] + b[1] * z + b[2] * z * z) / (1.0 + a[0] * z + a[1] * z * z)
    db = 20.0 * np.log10(np.maximum(np.abs(h), 1e-12))
    return np.maximum(db - db[0], FLOOR_DB)


def fluidsynth_db(cents, resonance):
    """Compute FluidSynth filter coefficients for cutoff in absolute cents and resonance in cB."""
    fres = min(max(8.176 * 2.0 ** (cents / 1200.0), 5.0), 0.45 * OUT_RATE)
    q = 10.0 ** ((min(max(resonance / 10.0, 0.0), 96.0) - 3.01) / 20.0)
    w = 2.0 * np.pi * fres / OUT_RATE
    alpha = np.sin(w) / (2.0 * q)
    b1 = (1.0 - np.cos(w)) / (1.0 + alpha)
    return response_db((b1 / 2.0, b1, b1 / 2.0), (-2.0 * np.cos(w) / (1.0 + alpha), (1.0 - alpha) / (1.0 + alpha)),
                       OUT_RATE)


def best_cutoff(target):
    """Find the zero-resonance SoundFont cutoff closest to `target`. Return cutoff in absolute cents and RMS
    error in dB."""
    errors = [np.sqrt(np.mean((fluidsynth_db(c, 0) - target) ** 2)) for c in CUTOFFS]
    i = int(np.argmin(errors))
    return int(CUTOFFS[i]), float(errors[i])


def table(ctype, name, comment, values, per_line=12):
    """Format a C++ table with `per_line` values per line, followed by a blank line."""
    lines = [f"// {comment}", f"constexpr {ctype} {name}[{len(values)}] = {{"]
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="Generate the filter tables for src/filter.cpp.")
    ap.add_argument("dol", type=Path, help="Wii Music's sys/main.dol")
    args = ap.parse_args()
    dol = dol_sections(args.dol)
    first = read(dol, ">5h", BIQUADS[1][0])
    if first[0] != first[2] or first[1] != 2 * first[0]:
        sys.exit(f"{args.dol} isn't Wii Music's main.dol (the biquad low-pass's first row reads {first})")

    arguments = [a for a in range(64) if cutoff_hz(a) < OFF]
    sf2, sfz, errors = [], [], []
    for argument in arguments:
        a0, b0 = one_pole(cutoff_hz(argument))
        cutoff, error = best_cutoff(response_db((a0 / 32768.0, 0.0, 0.0), (-b0 / 32768.0, 0.0), GAME_RATE))
        sf2.append(cutoff)
        errors.append(error)
        pole_hz = -GAME_RATE * math.log(b0 / 32768.0) / (2.0 * math.pi)
        sfz.append(int(round(1200.0 * math.log2(pole_hz / SFZ_LOW_PASS_BASE))))

    print(table("uint16_t", "kLowPassHz", f"AX's cutoffs in Hz, arguments 0 to {arguments[-1]}.",
                [cutoff_hz(a) for a in arguments]))
    print(table("uint16_t", "kLowPassCutoff", f"The SoundFont cutoffs, arguments 0 to {arguments[-1]}.", sf2))
    print(table("int16_t", "kSfzLowPassCutoff", f"The SFZ cutoffs, arguments 0 to {arguments[-1]}.", sfz))

    # The biquad presets: cutoff (cents from SFZ_BIQUAD_BASE), Q (0.01 dB) and gain (0.01 dB) of each step.
    for kind, (address, sets, name) in BIQUADS.items():
        steps = [decode_biquad(kind, read(dol, ">5h", address + 10 * i)) for i in range(sets)]
        values = []
        for freq, q, gain in steps:
            cutoff, resonance = cents(freq / SFZ_BIQUAD_BASE), round(2000.0 * math.log10(q))
            values.append(f"{{{cutoff}, {resonance}, {round(100.0 * gain)}}}")
        print(table("SfzBiquad", f"kSfzBiquad{kind}", f"Type {kind}, {name}: steps 0 to {sets - 1}.", values, 5))
        if kind == 1:
            sf2 = [f"{{{cents(freq / 8.176)}, {round(10.0 * (20.0 * math.log10(q) + 3.01))}}}" for freq, q, _ in steps]
            print(table("SoundFontFilter", "kBiquadLowPass", "The SoundFont filters, steps 0 to 111.", sf2, 6))
    print(f"// RMS difference, SoundFont low-pass: {min(errors):.2f} to {max(errors):.2f} dB", file=sys.stderr)


if __name__ == "__main__":
    main()
