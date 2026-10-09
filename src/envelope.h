// SPDX-License-Identifier: MIT

// NW4R SND envelope (ADSHR) timing and its SoundFont 2 and SFZ equivalents.

#pragma once

#include <cstdint>

namespace revosf2
{

// Sound frame duration in milliseconds: one AX frame, 96 samples at 32000 Hz. NW4R SND's sound thread runs once per
// frame.
constexpr double kFrameMs = 3.0;

// Envelope, LFO and timed sweep advance per sound frame, in milliseconds: the frame's real time.
constexpr int kEnvStepMs = 3;

// NW4R SND's silence threshold, -90.4 dB, in 0.1 dB units. Attack starts here; a released note stops when its level
// reaches it.
constexpr float kSilentLevel = -904.0f;

// Attack duration from -90.4 dB to full level, in milliseconds.
double AttackMs(int attack);

// Hold duration in milliseconds. Builds of NW4R SND before 2008 have no hold. A hold of 0 gives the same.
double HoldMs(int hold);

// Decay or release rate for an MML value, in 0.1 dB per millisecond.
double DecayRate(int value);

// Sustain level in 0.1 dB (<= 0).
int SustainCentibels(int sustain);

// MML envelope parameters. A hold of 0 holds for no time.
struct EnvelopeValues
{
    int attack = 127, decay = 127, sustain = 127, hold = 0, release = 127;
};

// Release starting level after `frames` envelope updates, in 0.1 dB units. Before the first update it is -90.4 dB, even
// for an instant attack.
float HeldLevel(const EnvelopeValues& v, int64_t frames);

// True if the game plays an envelope at `level` (0.1 dB units) as silence (Util::CalcVolumeRatio, main.dol 0x8004e9cc,
// gives 0). Silence stops a released note.
bool Silent(float level);

// Count release updates until a released note at `level` (0.1 dB units) falls silent and stops. `rate` is positive, in
// 0.1 dB per millisecond (see DecayRate).
int64_t ReleaseFrames(float level, double rate);

// Count envelope updates until a held note has attacked and then decayed to `level` or below (0.1 dB units). Return -1
// if it never reaches that level.
int64_t FramesToLevel(const EnvelopeValues& v, float level);

// Treat sustain attenuation at or above this value as inaudible (centibels).
constexpr int kInaudibleSustain = 400;

// The attenuation in centibels down to which the MIDI note of a channel that its length can't release follows the
// channel's fade. A loud note can still be heard at kInaudibleSustain.
constexpr int kFadedAttenuation = 600;

// SoundFont volume envelope generators (timecents; sustain in centibels of attenuation).
struct Sf2Envelope
{
    int attack = -12000, hold = -12000, decay = -12000, sustain = 0, release = -12000;
};

// Convert the game's envelope timing. Fit a linear SoundFont attack to the game's attack curve.
Sf2Envelope ConvertEnvelope(const EnvelopeValues& v);

// The time the SoundFont and SFZ attacks take to reach full level, in milliseconds. Both rise linearly in amplitude.
double Sf2AttackMs(const EnvelopeValues& v);

// Convert milliseconds to SoundFont timecents, clamped to lo..hi. Times <= 1 ms return lo.
int ToTimecents(double ms, int lo, int hi);

// SFZ amplitude envelope (ampeg_*): times in seconds, sustain as a percentage of full amplitude.
struct SfzEnvelope
{
    double attack = 0.0, hold = 0.0, decay = 0.0, sustain = 100.0, release = 0.0;
};

// Convert to sfizz's SFZ envelope, with the same attack fit as SoundFont. As in the game, decay and release are linear
// in dB. They fall by a factor of e^-9 (78.17 dB) over their specified duration.
SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& v);

} // namespace revosf2
