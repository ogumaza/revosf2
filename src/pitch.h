// SPDX-License-Identifier: MIT

// NW4R SND pitch ratios (1/256-semitone units) and LFO.

#pragma once

#include <cstdint>

namespace revosf2
{

// Convert `pitch` from 1/256 semitones to a frequency ratio, following Util::CalcPitchRatio (main.dol 0x8004e83c).
// Apply whole octaves, then single-precision 2^(i/12) and 2^(i/3072) tables for the remainder.
float PitchRatio(int pitch);

// NW4R SND's LFO: a delayed sine from a quarter-wave table (Lfo::Update, main.dol 0x8003b280, and GetValue,
// 0x8003b31c).
class Lfo
{
public:
    // Set MML depth, speed and range, with delay in milliseconds.
    void Set(uint8_t depth, uint8_t speed, uint8_t range, uint32_t delay_ms);

    // Advance by `msec` milliseconds. Use up the delay first.
    void Update(int msec);

    // Return depth / 128 * range * sine, in semitones for a pitch LFO.
    float Value() const;

private:
    float depth_ = 0.0f;         // MML depth / 128
    float speed_ = 6.25f;        // Hz: MML speed * 100 / 256
    uint32_t delay_ = 0;         // milliseconds
    uint8_t range_ = 1;          // semitones, for a pitch LFO
    uint32_t delay_counter_ = 0; // elapsed delay in milliseconds
    float phase_ = 0.0f;         // 0-1
};

} // namespace revosf2
