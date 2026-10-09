// SPDX-License-Identifier: MIT

// The track low-pass filter and its SoundFont/SFZ equivalents.
//
// A track has a low-pass filter (0xd8). NW4R SND updates it on sounding notes every frame
// (SeqTrack::UpdateChannelParam, main.dol 0x800403e0) and plays it on AX's one-pole filter. The cutoff is 32000 * 2^(10
// (argument - 64) / 64) Hz. From 16 kHz up, at arguments of 58 or more, the filter is off. Builds of NW4R SND from June
// 2008 add a biquad filter (0xb4 type, 0xb5 value). The biquad selects coefficients from five preset tables: one
// low-pass, one high-pass and three band-passes, all Robert Bristow-Johnson cookbook filters. Wii Music's tables are
// the same as the Wii U's tables.
//
// SoundFont provides one resonant two-pole low-pass. It matches the biquad low-pass and approximates the one-pole
// filter with fitted cutoffs. High-pass and band-pass are omitted. When both low-passes are active, use the lower
// cutoff. SFZ supports all these filters: sfizz's lpf_1p matches AX's one-pole, and lpf_2p, hpf_2p and bpf_2p are
// cookbook filters. Two MIDI controllers hold the track's settings.
//
// Generate filter.cpp's tables with tools/filter_tables.py.

#pragma once

#include <cstdint>

namespace revosf2
{

// Open SoundFont filter cutoff (initialFilterFc, absolute cents).
constexpr uint16_t kFilterOpen = 13500;

// SoundFont cutoff controller for filters that change during a note. A modulator subtracts kFilterControllerRange *
// value / 128 cents from kFilterOpen: 75 cents per step. Zero leaves the filter open.
constexpr uint8_t kFilterController = 102;
constexpr int kFilterControllerRange = 9600;

// Returns the value of kFilterController that comes closest to `cutoff`.
uint8_t FilterControllerValue(uint16_t cutoff);

// SFZ filter controllers. Low-pass uses 64 minus the 0xd8 argument (6 or less means off). Biquad uses the 0xb5
// argument, or 0 for types without a preset. The default controller value of 0 disables both.
constexpr uint8_t kSfzLowPassController = 103;
constexpr uint8_t kSfzBiquadController = 104;

// Returns the value of kSfzLowPassController for the 0xd8 argument `low_pass`.
uint8_t SfzLowPassValue(int low_pass);

// Returns the value of kSfzBiquadController for the 0xb4 and 0xb5 arguments `biquad_type` and `biquad_value`.
uint8_t SfzBiquadValue(int biquad_type, int biquad_value);

// The row of biquad preset `biquad_type` that 0xb5 argument `biquad_value` selects, or -1 for no biquad.
int BiquadStep(int biquad_type, int biquad_value);

// The low-pass's setting for the 0xd8 argument `low_pass`: 0 to 57, from a cutoff of 31 Hz to 14993 Hz, or -1 for off.
int LowPassIndex(int low_pass);

// AX's cutoff in Hz for low-pass setting `index` (0-57).
int LowPassHz(int index);

// A SoundFont filter setting.
struct SoundFontFilter
{
    bool operator==(const SoundFontFilter&) const = default;

    uint16_t cutoff = kFilterOpen; // initialFilterFc, in absolute cents
    uint8_t resonance = 0;         // initialFilterQ, in centibels
};

// Convert the 0xd8 argument `low_pass`, and the biquad's 0xb4 and 0xb5 arguments `biquad_type` and `biquad_value`, to a
// SoundFont filter approximation.
SoundFontFilter ToSoundFont(int low_pass, int biquad_type, int biquad_value);

// Open SFZ low-pass cutoff, lowered by kSfzLowPassController. This is sfizz's maximum for lpf_1p.
constexpr int kSfzLowPassOpen = 20000; // Hz

// Return the SFZ lpf_1p cutoff matching low-pass setting `index` (0-57), in cents relative to kSfzLowPassOpen.
int SfzLowPassCutoff(int index);

// Reference frequency for SFZ biquad cutoffs.
constexpr int kSfzBiquadBase = 1000; // Hz

// An SFZ biquad setting: cutoff/centre frequency in cents relative to kSfzBiquadBase, plus resonance (Q in dB) and gain
// in 0.01 dB units.
struct SfzBiquad
{
    int16_t cutoff = 0;
    int16_t resonance = 0;
    int16_t gain = 0;
};

// Returns biquad preset `type` (1-5) at row `step` as an SFZ filter.
SfzBiquad SfzBiquadStep(int type, int step);

} // namespace revosf2
