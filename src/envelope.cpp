// SPDX-License-Identifier: MIT

// NW4R SND ADSHR envelopes (EnvGenerator) and their SoundFont 2/SFZ conversions. The tables and arithmetic follow Wii
// Sports' main.dol, and the hold that came with the builds of 2008 follows Mario Kart Wii's.

#include "envelope.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace revosf2
{

namespace
{

// Per-millisecond attack multipliers of the level in dB (EnvGenerator::SetAttack's table, main.dol 0x80375940). They're
// nw::snd's on the 3DS, bit for bit.
constexpr float kAttackTable[128] = {
    0x1.ff99700000000p-1f, 0x1.ff328e0000000p-1f, 0x1.fecb5a0000000p-1f, 0x1.fe63d20000000p-1f,
    0x1.fdfbf40000000p-1f, 0x1.fd93c20000000p-1f, 0x1.fd2b3a0000000p-1f, 0x1.fcc25e0000000p-1f,
    0x1.fc59280000000p-1f, 0x1.fbef9e0000000p-1f, 0x1.fb85ba0000000p-1f, 0x1.fb1b7e0000000p-1f,
    0x1.fab0e60000000p-1f, 0x1.fa45fa0000000p-1f, 0x1.f9daae0000000p-1f, 0x1.f96f080000000p-1f,
    0x1.f903060000000p-1f, 0x1.f896a80000000p-1f, 0x1.f829ee0000000p-1f, 0x1.f7bcd40000000p-1f,
    0x1.f74f5a0000000p-1f, 0x1.f6e1840000000p-1f, 0x1.f673480000000p-1f, 0x1.f604b00000000p-1f,
    0x1.f595b60000000p-1f, 0x1.f526560000000p-1f, 0x1.f4b6960000000p-1f, 0x1.f446700000000p-1f,
    0x1.f3d5e60000000p-1f, 0x1.f364f80000000p-1f, 0x1.f2f3a20000000p-1f, 0x1.f281e20000000p-1f,
    0x1.f20fbc0000000p-1f, 0x1.f19d2e0000000p-1f, 0x1.f12a340000000p-1f, 0x1.f0b6d20000000p-1f,
    0x1.f043020000000p-1f, 0x1.efcec40000000p-1f, 0x1.ef5a1e0000000p-1f, 0x1.eee5040000000p-1f,
    0x1.ee6f800000000p-1f, 0x1.edf9880000000p-1f, 0x1.ed83200000000p-1f, 0x1.ed0c440000000p-1f,
    0x1.ec94f60000000p-1f, 0x1.ec1d380000000p-1f, 0x1.eba5020000000p-1f, 0x1.eb2c540000000p-1f,
    0x1.eab3320000000p-1f, 0x1.ea39960000000p-1f, 0x1.e9bf820000000p-1f, 0x1.e944f40000000p-1f,
    0x1.e8c9ea0000000p-1f, 0x1.e84e640000000p-1f, 0x1.e7d2600000000p-1f, 0x1.e755de0000000p-1f,
    0x1.e6d8da0000000p-1f, 0x1.e65b5a0000000p-1f, 0x1.e5dd540000000p-1f, 0x1.e55eca0000000p-1f,
    0x1.e4dfbe0000000p-1f, 0x1.e4602a0000000p-1f, 0x1.e3e0120000000p-1f, 0x1.e35f700000000p-1f,
    0x1.e2de440000000p-1f, 0x1.e25c8e0000000p-1f, 0x1.e1da4c0000000p-1f, 0x1.e1577c0000000p-1f,
    0x1.e0d4200000000p-1f, 0x1.e050300000000p-1f, 0x1.dfcbb00000000p-1f, 0x1.df469c0000000p-1f,
    0x1.dec0f60000000p-1f, 0x1.de3ab60000000p-1f, 0x1.ddb3e40000000p-1f, 0x1.dd2c740000000p-1f,
    0x1.dca46e0000000p-1f, 0x1.dc1bca0000000p-1f, 0x1.db92880000000p-1f, 0x1.db08a80000000p-1f,
    0x1.da7e240000000p-1f, 0x1.d9f3020000000p-1f, 0x1.d967380000000p-1f, 0x1.d8daca0000000p-1f,
    0x1.d84db40000000p-1f, 0x1.d7bff60000000p-1f, 0x1.d7318a0000000p-1f, 0x1.d6a2720000000p-1f,
    0x1.d612ac0000000p-1f, 0x1.d582380000000p-1f, 0x1.d4f10e0000000p-1f, 0x1.d45f300000000p-1f,
    0x1.d3cc980000000p-1f, 0x1.d3394c0000000p-1f, 0x1.d2a5440000000p-1f, 0x1.d2107e0000000p-1f,
    0x1.d17afc0000000p-1f, 0x1.d0e4b60000000p-1f, 0x1.d04db00000000p-1f, 0x1.cfb5e00000000p-1f,
    0x1.cf1d4c0000000p-1f, 0x1.ce83ea0000000p-1f, 0x1.cde9c00000000p-1f, 0x1.cd4ec60000000p-1f,
    0x1.ccb2f80000000p-1f, 0x1.cc165a0000000p-1f, 0x1.cb78e00000000p-1f, 0x1.cada920000000p-1f,
    0x1.ca3b660000000p-1f, 0x1.c7b5ee0000000p-1f, 0x1.c3d2160000000p-1f, 0x1.c079340000000p-1f,
    0x1.bd05f60000000p-1f, 0x1.ba2f1a0000000p-1f, 0x1.b508480000000p-1f, 0x1.afa02c0000000p-1f,
    0x1.a840080000000p-1f, 0x1.a13bde0000000p-1f, 0x1.99b6120000000p-1f, 0x1.8e5eb40000000p-1f,
    0x1.82cd9e0000000p-1f, 0x1.72cbfc0000000p-1f, 0x1.5d9b180000000p-1f, 0x1.440db00000000p-1f,
    0x1.1e513c0000000p-1f, 0x1.d210820000000p-2f, 0x1.51cb460000000p-2f, 0.0f,
};

// The sustain level for each MML value, round(200 log10((s/127)^2)) in 0.1 dB with the two lowest entries pinned
// (EnvGenerator's DecibelSquareTable, main.dol 0x80375840).
constexpr int16_t kSustainTable[128] = {
    -723, -722, -721, -651, -601, -562, -530, -503, -480, -460, -442, -425, -410, -396, -383, -371, -360, -349, -339,
    -330, -321, -313, -305, -297, -289, -282, -276, -269, -263, -257, -251, -245, -239, -234, -229, -224, -219, -214,
    -210, -205, -201, -196, -192, -188, -184, -180, -176, -173, -169, -165, -162, -158, -155, -152, -149, -145, -142,
    -139, -136, -133, -130, -127, -125, -122, -119, -116, -114, -111, -109, -106, -103, -101, -99,  -96,  -94,  -91,
    -89,  -87,  -85,  -82,  -80,  -78,  -76,  -74,  -72,  -70,  -68,  -66,  -64,  -62,  -60,  -58,  -56,  -54,  -52,
    -50,  -49,  -47,  -45,  -43,  -42,  -40,  -38,  -36,  -35,  -33,  -31,  -30,  -28,  -27,  -25,  -23,  -22,  -20,
    -19,  -17,  -16,  -14,  -13,  -11,  -10,  -8,   -7,   -6,   -4,   -3,   -1,   0,
};

// SoundFont attack is linear in amplitude; NW4R SND's attack is exponential in dB. The game's attack stays quiet longer
// before rising sharply. A ramp lasting 0.55 of the game's attack gives the lowest mean dB error at floors of -40, -60
// and -90 dB. The curve has the same shape for every attack value. The fit therefore applies to all of them.
//
// At that point, NW4R SND is within 0.5 dB of full level. Add the remaining time to the hold so decay starts on time.
// An initial envelope delay would fit the start better, but FluidSynth delays sample playback too.
constexpr double kAttackRamp = 0.55;

// SoundFont's slowest decay or release, 8000 timecents, in milliseconds for a 100 dB fall. MML values 0 to 2 are
// slower.
constexpr double kSlowestSf2DecayMs = 101593.6673259648;

// Attack ends above -0.003 dB. The value is in 0.1 dB units.
constexpr float kAttackEnd = -0.03125f;

// Per-millisecond attack multiplier for an MML attack value.
float AttackMultiplier(int attack)
{
    return kAttackTable[std::clamp(attack, 0, 127)];
}

// NW4R SND's EnvGenerator for held notes (attack, hold, decay, sustain), updated by 3 ms each sound frame.
class EnvelopeSim
{
public:
    explicit EnvelopeSim(const EnvelopeValues& v);

    // Advance by `msec` milliseconds.
    void Update(int msec);

    // Level in 0.1 dB.
    float Value() const;

    // Level at release, in 0.1 dB units. An instant attack's Value() is full level before the first update, as in NW4R
    // SND, but its internal level is still -90.4 dB.
    float RawValue() const;

private:
    enum class Status
    {
        kAttack,
        kHold,
        kDecay,
        kSustain
    };

    float attack_, decay_;
    int sustain_cb_;
    int hold_, hold_counter_ = 0;
    float value_ = kSilentLevel;
    Status status_ = Status::kAttack;
};

EnvelopeSim::EnvelopeSim(const EnvelopeValues& v)
    : attack_(AttackMultiplier(v.attack)),
      decay_(static_cast<float>(DecayRate(v.decay))),
      sustain_cb_(SustainCentibels(v.sustain)),
      hold_(static_cast<int>(HoldMs(v.hold)))
{
}

void EnvelopeSim::Update(int msec)
{
    // EnvGenerator::Update (main.dol 0x80038fdc; with the hold, Mario Kart Wii's 0x80091700). The attack's last frame
    // ends when it does: the hold, or the decay, starts with the next frame.
    switch (status_)
    {
    case Status::kAttack:
        for (int i = 0; i < msec; i++)
        {
            value_ *= attack_;
            if (value_ > kAttackEnd)
            {
                value_ = 0.0f;
                status_ = Status::kHold;
                hold_counter_ = hold_;
                return;
            }
        }
        return;

    case Status::kHold:
        if (hold_counter_ > msec)
        {
            hold_counter_ -= msec;
            return;
        }

        msec -= hold_counter_;
        hold_counter_ = 0;
        status_ = Status::kDecay;
        [[fallthrough]];

    case Status::kDecay:
        value_ -= decay_ * static_cast<float>(msec);
        if (value_ > static_cast<float>(sustain_cb_))
        {
            return;
        }

        value_ = static_cast<float>(sustain_cb_);
        status_ = Status::kSustain;
        return;

    case Status::kSustain:
        return;
    }
}

float EnvelopeSim::Value() const
{
    if (status_ != Status::kAttack || attack_ != 0.0f)
    {
        return value_;
    }

    return 0.0f;
}

float EnvelopeSim::RawValue() const
{
    return value_;
}

} // namespace

double AttackMs(int attack)
{
    // Start at -90.4 dB and multiply once per millisecond until above -0.003 dB. Every multiplier is below 1. The
    // attack therefore always finishes.
    const float a = AttackMultiplier(attack);
    float value = kSilentLevel;
    for (int ms = 1;; ms++)
    {
        value *= a;
        if (value > kAttackEnd)
        {
            return ms;
        }
    }
}

double HoldMs(int hold)
{
    // EnvGenerator::SetHold (Mario Kart Wii, 0x80091880).
    return static_cast<double>((hold + 1) * (hold + 1) / 4);
}

double DecayRate(int value)
{
    // Rate in 0.1 dB per millisecond. Keep the game's single-precision arithmetic (EnvGenerator::SetDecay, main.dol
    // 0x80039118, and SetRelease, 0x800391c0). NW4R SND divides by 5 where nw::snd multiplies by 0.2.
    if (value >= 127)
    {
        return 65535.0;
    }
    if (value == 126)
    {
        return 24.0;
    }
    if (value < 50)
    {
        return static_cast<float>(value * 2 + 1) * 0.0078125f / 5.0f;
    }

    return 60.0f / static_cast<float>(126 - value) / 5.0f;
}

int SustainCentibels(int sustain)
{
    return kSustainTable[std::clamp(sustain, 0, 127)];
}

float HeldLevel(const EnvelopeValues& v, int64_t frames)
{
    EnvelopeSim env(v);
    const float sustain = static_cast<float>(SustainCentibels(v.sustain));
    for (int64_t i = 0; i < frames; i++)
    {
        env.Update(kEnvStepMs);
        if (env.Value() == sustain)
        {
            break; // sustaining: the level no longer changes
        }
    }

    return env.RawValue();
}

int64_t FramesToLevel(const EnvelopeValues& v, float level)
{
    if (static_cast<float>(SustainCentibels(v.sustain)) > level)
    {
        return -1;
    }

    // The attack starts at -90.4 dB. It must rise above `level` before a later fall counts as decay to that level.
    EnvelopeSim env(v);
    bool risen = false;
    for (int64_t i = 1; i < 1000000; i++)
    {
        env.Update(kEnvStepMs);
        if (env.Value() > level)
        {
            risen = true;
        }
        else if (risen)
        {
            return i;
        }
    }

    return -1;
}

bool Silent(float level)
{
    // EnvGenerator::GetValue (0x80038fb0) gives the level in dB, and CalcVolumeRatio clamps it to -90.4 dB and looks up
    // its tenths of a dB, truncated, in a table whose first entry, for -90.4 dB, is 0.
    const float db = std::max(level / 10.0f, -90.4f);
    return static_cast<int>(10.0f * db) <= -904;
}

int64_t ReleaseFrames(float level, double rate)
{
    // Channel::Update (main.dol 0x800378c8) stops a released note at the start of the first frame whose level is
    // silent. Release 127 crosses the entire range in one frame; release 0 takes about 190,000 frames.
    const float step = static_cast<float>(rate) * static_cast<float>(kEnvStepMs);
    if (!(step > 0.0f))
    {
        return std::numeric_limits<int32_t>::max();
    }

    int64_t frames = 0;
    while (!Silent(level))
    {
        level -= step;
        frames++;
    }

    return frames;
}

Sf2Envelope ConvertEnvelope(const EnvelopeValues& v)
{
    Sf2Envelope e;
    const double attack_ms = v.attack < 127 ? AttackMs(v.attack) : 0.0;
    e.attack = ToTimecents(Sf2AttackMs(v), -12000, 8000);

    // SoundFont decay and release times specify a 100 dB fall, linear in dB. A decay slower than SoundFont allows gets
    // a longer hold, up to the longest hold SoundFont allows. Holding until the faster decay reaches the sustain level
    // when the game's decay does would leave the note too loud until then. The error would be largest at the end of the
    // hold. Half that hold splits the error evenly: as loud at the end of the hold as quiet when the decay reaches the
    // sustain level.
    const double decay_ms = 1000.0 / DecayRate(v.decay);
    double hold_ms = HoldMs(v.hold) + attack_ms * (1.0 - kAttackRamp);
    if (decay_ms > kSlowestSf2DecayMs)
    {
        hold_ms += -SustainCentibels(v.sustain) / 1000.0 * (decay_ms - kSlowestSf2DecayMs) / 2.0;
    }
    e.hold = ToTimecents(hold_ms, -12000, 5000);
    e.decay = ToTimecents(decay_ms, -12000, 8000);
    e.sustain = std::clamp(-SustainCentibels(v.sustain), 0, 1440);
    e.release = ToTimecents(1000.0 / DecayRate(v.release), -12000, 8000);

    return e;
}

double Sf2AttackMs(const EnvelopeValues& v)
{
    return v.attack < 127 ? AttackMs(v.attack) * kAttackRamp : 0.0;
}

SfzEnvelope ConvertEnvelopeForSfz(const EnvelopeValues& v)
{
    // sfizz multiplies amplitude by exp(-9 / (time * rate)) per sample: a fall of 9 * 200 / ln(10) tenths of a dB over
    // the decay or release time.
    constexpr double kSfizzFall = 9.0 * 200.0 / 2.302585092994046;

    SfzEnvelope e;
    const double attack_ms = v.attack < 127 ? AttackMs(v.attack) : 0.0;
    e.attack = Sf2AttackMs(v) / 1000.0;
    e.hold = (HoldMs(v.hold) + attack_ms * (1.0 - kAttackRamp)) / 1000.0;
    e.decay = kSfizzFall / DecayRate(v.decay) / 1000.0;
    e.sustain = 100.0 * std::pow(10.0, SustainCentibels(v.sustain) / 200.0);
    e.release = kSfizzFall / DecayRate(v.release) / 1000.0;

    return e;
}

int ToTimecents(double ms, int lo, int hi)
{
    if (ms <= 1.0)
    {
        return lo;
    }

    const double tc = 1200.0 * std::log2(ms / 1000.0);
    return std::clamp(static_cast<int>(std::lround(tc)), lo, hi);
}

} // namespace revosf2
