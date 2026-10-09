// SPDX-License-Identifier: MIT

// Convert NW4R SND pan to the SoundFont pan law.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace revosf2
{

// NW4R SND's default pan law is left = sqrt((1 - p) / 2), right = sqrt((1 + p) / 2), for p in -1..1 (Util's square-root
// pan table, main.dol 0x80376f3c, in 256 steps). FluidSynth and the SoundFont 2 reference implementation use a sin/cos
// law: right = sin((p' + 1) * pi / 4). Both preserve power. Matching their left/right ratios also matches their gains:
//
// p' = 4 / pi * atan(sqrt((1 + p) / (1 - p))) - 1
inline double SoundFontPan(double p)
{
    p = std::clamp(p, -1.0, 1.0);
    if (p >= 1.0)
    {
        return 1.0;
    }

    return 4.0 / std::numbers::pi * std::atan(std::sqrt((1.0 + p) / (1.0 - p))) - 1.0;
}

// A wave channel's place in NW4R SND's stereo mix: its pan, -1 (left) to 1 (right), and its gain in dB.
struct ChannelPan
{
    double pan = 0.0;
    double gain_db = 0.0;
};

// Return the place of channel `channel` of a wave with `channels` channels, for a note at pan `pan`
// (Voice::CalcMixParam, main.dol 0x8004d590). A mono wave plays at `pan`. In dual mode, each channel of a stereo wave
// pans from its side. In balance mode, each channel stays on its side, at the level the pan law gives that side: -3 dB
// at a pan of 0. Dual mode plays the channel at full level there.
inline ChannelPan WaveChannelPan(double pan, std::size_t channel, std::size_t channels, bool balance)
{
    if (channels < 2)
    {
        return {pan, 0.0};
    }

    const double side = channel == 0 ? -1.0 : 1.0;
    if (!balance)
    {
        return {pan + side, 0.0};
    }

    const double level = (1.0 + side * std::clamp(pan, -1.0, 1.0)) / 2.0;
    return {side, level > 0.0 ? 10.0 * std::log10(level) : -144.0};
}

} // namespace revosf2
