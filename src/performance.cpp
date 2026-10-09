// SPDX-License-Identifier: MIT

// Sequence execution (see performance.h). The performer follows the WSF project's model of NW4R SND as linked into Wii
// Sports' main.dol: MmlParser::Parse (0x8003b8b8), CommandProc (0x8003bee8), SeqTrack::ParseNextTick (0x80040190),
// UpdateChannelLength (0x800400a8), NoteOn (0x80040bc0), Close (0x8003ffd8), SeqPlayer::UpdateTick (0x8003eec4) and
// Channel::Update (0x800378c8). Where the builds of 2008 differ, comments give Mario Kart Wii's or Wii Music's
// addresses.

#include "performance.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include "binary.h"
#include "envelope.h"
#include "instruments.h"
#include "pan.h"
#include "pitch.h"
#include "wave.h"

namespace revosf2
{

namespace
{

// Commands a track may run in one tick. NW4R SND parses until a command waits. A track that loops without waiting hangs
// the game. The performer stops such a track instead.
constexpr int kParseLimit = 100000;

constexpr double kNever = 1e300;       // sentinel time in milliseconds
constexpr double kMaxTailMs = 10000.0; // longest release tail kept after the end

// Repeat short loops, such as sustained sound effects, until the performance reaches this duration. Keep loop markers
// around the first pass.
constexpr double kMinLoopSeconds = 10.0;

// Longest performance, in seconds of music.
constexpr double kMaxSeconds = 900.0;

// Maximum duration after a held sound stops changing. This covers tracks waiting on endless looping notes and sequences
// whose timing has stopped.
constexpr double kHoldSeconds = 10.0;

// Shortest note that a track waiting it out holds until the game stops the sound. WSF's length analysis uses the same
// limit: 200 beats at the default timebase, minutes at a typical tempo. Games use such notes for sounds they stop
// themselves.
constexpr int32_t kHeldTicks = 9600;

// AX's output samples per frame, and its rate.
constexpr uint64_t kFrameSamples = 96;
constexpr float kDspRate = 32000.0f;

// AX voice count. Each NW4R SND channel uses one voice, or two for stereo. AX of October 2006 has 64.
constexpr int kVoices = 96;
constexpr int kVoicesOctober2006 = 64;

// The build of October 2006 counts time in 416ths of a tick (StartFrameOctober2006).
constexpr int kTickUnits = 416;

// One frame of the build of October 2006's count of time (SeqPlayer's update, 0x800e6588 in Smooth Moves' main.dol). It
// takes the frame's whole ticks out of `units`, then adds the tempo. The tempo ratio is 1. The frame then runs the
// ticks it took out. The timebase plays no part. A tick lasts 416 / tempo frames. Return the ticks.
int StartFrameOctober2006(uint16_t& units, int tempo)
{
    int ticks = 0;
    while (units >= kTickUnits)
    {
        ticks++;
        units = static_cast<uint16_t>(units - kTickUnits);
    }
    units = static_cast<uint16_t>(units + tempo);

    return ticks;
}

// True for controller events that update a track's channel settings.
bool IsTrackSetting(EventKind kind)
{
    switch (kind)
    {
    case EventKind::kVolume:
    case EventKind::kExpression:
    case EventKind::kPan:
    case EventKind::kPitch:
    case EventKind::kModulation:
    case EventKind::kReverb:
    case EventKind::kChorus:
    case EventKind::kFilter:
    case EventKind::kSfzLowPass:
    case EventKind::kSfzBiquad:
        return true;
    default:
        return false;
    }
}

// The game's runtime conversion of a float to an unsigned integer: 0 below 0, all ones from 2^32, and truncated
// between.
uint32_t ToUnsigned(float value)
{
    if (!(value >= 0.0f))
    {
        return 0;
    }
    if (!(value < 4294967296.0f))
    {
        return 0xffffffff;
    }

    return static_cast<uint32_t>(static_cast<double>(value));
}

// A shift as PowerPC's slw and sraw do it: amounts from 32 to 63 shift everything out, and the amount's lower 6 bits
// count.
int32_t ShiftLeft(int32_t value, int32_t amount)
{
    return (amount & 0x3f) >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(value) << (amount & 0x1f));
}

int32_t ShiftRight(int32_t value, int32_t amount)
{
    return (amount & 0x3f) >= 32 ? (value < 0 ? -1 : 0) : value >> (amount & 0x1f);
}

// A linear ramp over a number of ticks (MoveValue): how volume and pans move in builds from 2008, over the time a
// prefix gives (Mario Kart Wii's MmlParser::CommandProc, 0x800968d0). Older builds, and commands without a time, set
// them at once.
class MoveValue
{
public:
    // Set immediately.
    void Init(int v)
    {
        start_ = target_ = v;
        duration_ = counter_ = 0;
    }

    // Advance the ramp by one tick.
    void Update()
    {
        if (counter_ < duration_)
        {
            counter_++;
        }
    }

    int Get() const
    {
        if (counter_ >= duration_)
        {
            return target_;
        }

        return (target_ - start_) * counter_ / duration_ + start_;
    }

    bool Moving() const
    {
        return counter_ < duration_;
    }

    // Ramp from the current value to `t` over `ticks`. NW4R SND stores the count in 16 bits. Arguments >= 0x8000 are
    // negative there and apply immediately.
    void SetTarget(int t, int ticks)
    {
        start_ = Get();
        counter_ = 0;
        target_ = t;
        duration_ = std::max(0, static_cast<int>(static_cast<int16_t>(ticks)));
    }

private:
    int start_ = 0, target_ = 0, duration_ = 0, counter_ = 0;
};

// NW4R SND's random generator (Util::CalcRandom, main.dol 0x8004ec58).
class Random
{
public:
    explicit Random(uint32_t seed) : state_(seed)
    {
    }

    uint16_t Next()
    {
        state_ = state_ * 0x19660Du + 0x3C6EF35Fu;

        return static_cast<uint16_t>(state_ >> 16);
    }

private:
    uint32_t state_;
};

// A sounding NW4R SND channel and its corresponding MIDI note.
struct Note
{
    // Return the AX frame in which a one-shot wave runs past its end. AX plays a channel from the frame after its
    // note-on's sound frame, each frame at the pitch NW4R SND gave it in the sound frame before. A voice that doesn't
    // loop goes on to AX's zero buffer when it passes its end (WSF found this frame-exact against the DSP).
    uint64_t WaveEndFrame() const
    {
        if (per_frame == 0)
        {
            return std::numeric_limits<uint64_t>::max();
        }

        const uint64_t left = wave_length > position ? wave_length - position : 0;
        return start_frame + (left + per_frame - 1) / per_frame - 1;
    }

    // Return when a one-shot wave's channel is freed (never for a looping wave): the first frame whose ticks no longer
    // find the channel. AX takes the voice's position back from the DSP at the next frame's sync (__AXServiceVPB,
    // main.dol 0x80106814). Voice::StopFinished (0x8004bb24) then finds the voice in the zero buffer in the sound frame
    // after the AX frame in which the wave ran out. StopFinished frees the channel there, after the frame's ticks.
    double NaturalEndMs() const
    {
        if (!one_shot || per_frame == 0)
        {
            return kNever;
        }

        return static_cast<double>(WaveEndFrame() + 2) * kFrameMs;
    }

    // Return the earlier of wave completion and release completion.
    double EndMs() const
    {
        return std::min(NaturalEndMs(), release_end_ms);
    }

    // Return when the channel's sound ends: where its fade ended its MIDI note (`sunk`), and otherwise when the channel
    // ends.
    double SoundEndMs() const
    {
        return sunk ? faded_ms : EndMs();
    }

    // Return when the held envelope sinks below hearing (kInaudibleSustain), or kNever if it doesn't.
    double QuietMs() const
    {
        return AttenuationMs(kInaudibleSustain);
    }

    // Return when the held envelope fades to kFadedAttenuation. If its sustain level stays above that, return when it
    // sinks below hearing instead.
    double FadedMs() const
    {
        const double faded = AttenuationMs(kFadedAttenuation);
        return faded < kNever ? faded : QuietMs();
    }

    // Return when the held envelope falls to `attenuation` centibels, or kNever if it doesn't.
    double AttenuationMs(int attenuation) const
    {
        const int64_t frames = FramesToLevel(env, -static_cast<float>(attenuation));
        return frames >= 0 ? static_cast<double>(on_frame + static_cast<uint64_t>(frames) + 1) * kFrameMs : kNever;
    }

    // True if the one-shot wave has finished by the end of AX frame `frame`.
    bool WaveEndedBy(uint64_t frame) const
    {
        return one_shot && per_frame != 0 && WaveEndFrame() <= frame;
    }

    // Play from AX frame `frame + 1` on at `new_per_frame` 65536ths of a sample per frame, the rate NW4R SND sets in
    // sound frame `frame`. First account for the frames played at the old rate. Finished waves keep their end.
    void ChangeRate(uint64_t frame, uint64_t new_per_frame)
    {
        if (WaveEndedBy(frame))
        {
            return;
        }

        if (frame + 1 > start_frame)
        {
            position += (frame + 1 - start_frame) * per_frame;
            start_frame = frame + 1;
        }

        per_frame = new_per_frame;
    }

    // Start a one-shot wave of `samples` samples in sound frame `frame`, at `new_per_frame` 65536ths of a sample per AX
    // frame. The microcode moves a voice on by its 16.16 ratio for each of the frame's 96 samples.
    void StartWave(uint64_t frame, uint32_t samples, uint64_t new_per_frame)
    {
        one_shot = true;
        wave_length = uint64_t{samples} << 16;
        position = 0;
        start_frame = frame + 1;
        per_frame = new_per_frame;
    }

    // Calculate the sweep's offset in semitones for sound frame `frame` (Channel::Update). Timed sweeps advance 3 ms
    // per frame after pitch calculation, from the initial frame on. Note-length sweeps advance once per tick.
    float SweepAt(uint64_t frame) const
    {
        const int total = static_cast<int>(sweep_length);
        const int gone = sweep_in_ms ? static_cast<int>(std::min<uint64_t>((frame - sweep_frame) * kEnvStepMs, total))
                                     : static_cast<int>(std::min(sweep_ticks, sweep_length));
        if (sweep == 0.0f || gone >= total)
        {
            return 0.0f;
        }

        return sweep * static_cast<float>(total - gone) / static_cast<float>(total);
    }

    // True if `other`'s sweep moves its pitch as this note's does from sound frame `frame` on: neither sweeps any more,
    // or both sweep alike.
    bool SweepsLike(const Note& other, uint64_t frame) const
    {
        if (SweepAt(frame) == 0.0f && other.SweepAt(frame) == 0.0f)
        {
            return true;
        }

        return sweep == other.sweep && sweep_length == other.sweep_length && sweep_in_ms == other.sweep_in_ms &&
               (sweep_in_ms ? sweep_frame == other.sweep_frame : sweep_ticks == other.sweep_ticks);
    }

    // Calculate 96 times the voice's 16.16 ratio at `cent` semitones from the region's original key: Channel::Update
    // converts the pitch to a ratio and applies the region's tuning, and AxVoice::SetSrc (main.dol 0x80033fd0) scales
    // it by the sample rate over AX's 32 kHz. All arithmetic is single precision.
    uint64_t RatePerFrame(float cent) const
    {
        const float ratio = PitchRatio(static_cast<int>(256.0f * cent)) * tune;
        const float dsp_ratio = ratio * static_cast<float>(sample_rate) / kDspRate;
        return kFrameSamples * ToUnsigned(65536.0f * dsp_ratio);
    }

    // Channel::Update's pitch for sound frame `frame`, in semitones from the original key.
    float Cent(uint64_t frame) const
    {
        float cent = 0.0f;
        cent += static_cast<float>(key - original_key);
        cent += SweepAt(frame);
        cent += bend;
        if (lfo_type == 0)
        {
            cent += lfo.Value();
        }

        return cent;
    }

    // True if the channel has a MIDI note. The game plays a note at velocity 0 silently, and MIDI gets no note for it.
    // The MIDI note of a channel that its length can't release ends where the channel's fade ends it (`sunk`). The
    // checks of a part's notes leave out a channel without a MIDI note (Alone, PartSounds, PartPitch and others).
    // Otherwise a silent channel's sweep and settings would move the notes that sound on the same part.
    bool HasMidiNote() const
    {
        return midi_id != 0 && !sunk;
    }

    uint8_t key = 0;           // the key NW4R SND plays
    uint8_t midi_key = 0;      // the key of the MIDI note
    bool midi_on = false;      // the MIDI note is on
    uint8_t midi_velocity = 0; // the MIDI note's velocity
    int start_velocity = 0;    // the velocity NW4R SND played the MIDI note's start at
    bool liftable = false;     // the MIDI note started alone on its part: a later pass may start it higher
    uint8_t part = 0;          // the part of the track the MIDI note is on (TrackEvent::part)
    uint8_t midi_volume = 127; // volume of the region selected for the MIDI note (see Legato)
    uint32_t midi_id = 0;      // the MIDI note's id (TrackEvent::note)
    uint64_t midi_frame = 0;   // the sound frame the MIDI note started in
    std::optional<int16_t> attack_timecents; // the MIDI note's lengthened attack (PresetKey::attack_timecents)
    PresetKey preset;                        // the MIDI note's preset, for its LFO settings (CheckLfo)
    bool lfo_moved = false;                  // reported as heard with other LFO settings than its preset has
    int32_t length = -1;                     // ticks until note-off; -1: none
    bool released = false;
    bool ignore_note_off = false;
    double quiet_ms = kNever; // time the held envelope sinks below hearing (QuietMs)
    double faded_ms = kNever; // time the MIDI note of a channel that its length can't release ends (FadedMs)
    bool sunk = false;        // its MIDI note ended below hearing (UpdateNoteLengths)
    uint8_t key_group = 0;
    uint64_t on_frame = 0; // the sound frame the note started in
    EnvelopeValues env;
    double release_end_ms = kNever;
    uint64_t released_frame = 0; // the sound frame the note was last released in
    float release_level = 0;     // 0.1 dB when released
    double release_rate = 0;     // 0.1 dB per millisecond

    // The channel's volume before the track's (Channel::SetInitVolume): velocity squared, and the region's volume for a
    // new note. The voice clamps it times the track's volumes to 1 (Voice::SetVolume, main.dol 0x8004c830).
    float init_volume = 1.0f;
    bool stereo = false;
    bool over_full_level = false; // reported as played below the game's level (CheckFullLevel)

    // One-shot playback: the voice's position at the start of AX frame `start_frame`, the position's advance per frame
    // (`per_frame`) and the wave's length, all in 65536ths of a sample.
    bool one_shot = false;
    uint64_t wave_length = 0, position = 0, per_frame = 0;
    uint64_t start_frame = 0;

    // Instrument and initial pan at note-on. Tie/mono continue the channel with these values even if the track changes
    // them.
    uint32_t program = 0;
    int8_t init_pan = 0;

    uint8_t original_key = 60;
    float tune = 1.0f;
    uint32_t sample_rate = 32000;

    // Pitch inputs for Channel::Update, in addition to key and sweep: the track bend, its latest LFO settings (see
    // GiveTrackSettings), and the channel's LFO state.
    float bend = 0.0f;
    Lfo lfo;
    uint8_t lfo_type = 0;

    // Sweep/portamento starts `sweep` semitones from the target and returns linearly.
    float sweep = 0.0f;
    double sweep_length = 0; // ticks, or milliseconds when sweep_in_ms
    double sweep_ticks = 0;
    uint64_t sweep_frame = 0; // the sound frame a timed sweep starts in
    bool sweep_in_ms = false;
};

// The values last sent to a part of a track that the track has left (Performer::Switch), and the legato gain of its
// notes.
struct LeftPart
{
    int volume = -1, expression = -1, pan = -1, mod = -1, reverb = -1, chorus = -1, filter = -1, sfz_low_pass = -1,
        sfz_biquad = -1;
    float pitch = 0.0f;
    double gain = 1.0;
};

// NW4R SND SeqTrack state and the last values sent to its MIDI channel.
struct Track
{
    // The pitch bend in semitones (SeqTrack::UpdateChannelParam, main.dol 0x800403e0).
    float Bend() const
    {
        return static_cast<float>(bend) / 128.0f * static_cast<float>(bend_range);
    }

    // Reset to the initial sound state but keep track allocation.
    void Init()
    {
        const bool was_allocated = allocated;
        *this = Track{};
        allocated = was_allocated;

        vol.Init(127);
        pan.Init(0);
        surround_pan.Init(0);
        vars.fill(-1);
    }

    bool allocated = false;
    bool open = false;
    uint32_t current = 0;  // offset of the next command
    bool cmp_flag = true;  // result of the last comparison (tested by the if prefix)
    bool note_wait = true; // a note makes the track wait for its length (MML notewait)
    bool tie = false;
    bool mono = false; // builds from 2008
    std::array<uint32_t, 3> call_stack{};
    std::array<uint8_t, 3> loop_count{};
    int call_depth = 0;
    int32_t wait = 0; // ticks until the next command; negative waits for ever
    bool mute = false;
    bool note_finish_wait = false; // waiting for the track's notes to end
    bool held_wait = false;        // waiting out a note of kHeldTicks or more
    bool porta = false;
    bool damper = false;
    uint32_t program = 0;
    uint8_t lfo_depth = 0, lfo_speed = 16, lfo_range = 1, lfo_type = 0;
    int32_t lfo_delay = 0;    // 5 ms units; a negative delay prevents the LFO from starting
    float sweep_pitch = 0.0f; // a note's starting offset from its pitch, in semitones
    MoveValue vol, pan, surround_pan;
    int8_t bend = 0;
    uint8_t volume2 = 127;
    uint8_t velocity_range = 127; // builds from June 2008: velocities are scaled by this / 127
    uint8_t bend_range = 2;
    int8_t init_pan = 0;
    int8_t transpose = 0;
    uint8_t porta_key = 60; // the key portamento starts from
    uint8_t porta_time = 0; // 0: the sweep takes the note's length
    uint8_t attack = 0xff, decay = 0xff, sustain = 0xff, release = 0xff;
    uint8_t hold = 0xff; // builds from 2008
    uint8_t fx_send_a = 0, fx_send_b = 0, fx_send_c = 0;
    std::array<int16_t, 16> vars{}; // track variables, 32-47 to commands
    std::vector<Note> notes;        // the track's channels, newest last
    std::vector<Note> detached;     // released channels no longer on the track, still sounding

    // First tick of a run without waits (kParseLimit), last tick that began a wait (OnLoop), and counts of
    // channels/legatos started in `note_tick` (NoteOn).
    std::optional<uint32_t> last_wait;
    uint32_t note_tick = 0;
    int tick_notes = 0, tick_legatos = 0;

    // Track filter arguments: the low-pass (0xd8, initially off) and the biquad's type and value (0xb4/0xb5, builds
    // from June 2008). `filter` is the SoundFont approximation of both (ToSoundFont).
    int32_t low_pass = 0x40;
    int32_t biquad_type = 0, biquad_value = 0;
    SoundFontFilter filter;

    // Last MIDI controller values, valid after the track's first note.
    bool played = false;
    int out_preset = -1;
    int out_volume = -1, out_expression = -1, out_pan = -1, out_mod = -1, out_reverb = -1, out_chorus = -1,
        out_filter = -1;
    int out_sfz_low_pass = 0, out_sfz_biquad = 0; // default MIDI value; filter off
    float out_pitch = 0.0f;

    // Last tick that changed the track's sound, used for main-loop detection (OnLoop). This must agree across Perform's
    // passes even when legato MIDI output differs (Legato). Count every legato as a change. Compare the game's
    // unrounded level before legato gain, MIDI scaling or clamping; legato gain can make CC11 steps finer. Also compare
    // the filter cutoff independently of controller use, and pitch without the legato key offset. change_level,
    // change_filter and change_pitch store those values.
    std::optional<uint32_t> last_change;
    double change_level = -1.0;
    int change_filter = -1;
    float change_pitch = 0.0f;
    bool idle = false;   // its last pass of a loop changed nothing (OnLoop)
    uint32_t opened = 0; // the tick it last opened at

    // When the track last changed a player or global variable that another open track has read and can read again. The
    // value is the number of commands run by then (Performer::commands_run_). Such a change counts as a change of the
    // sound for main-loop detection (OnLoop). The other track may play because of it.
    std::optional<uint64_t> last_signal;

    // Legato velocity relative to the continuing MIDI note, and key offset in semitones (see Legato).
    double legato_gain = 1.0;
    float legato_bend = 0.0f;

    // The part the track's new notes and changes go to (Switch), and the parts it has left.
    uint8_t part = 0;
    std::array<LeftPart, 16> left{};
};

// Command argument encodings.
enum ArgType
{
    kArgNone,
    kArgU8,
    kArgS16,
    kArgVmidi,
    kArgRandom,
    kArgVariable
};

// Build a preset key for a new note on `tr`. Envelope values outside 0-127 leave the bank value unchanged. Discard
// settings for ineffective LFOs to avoid duplicate presets. These include unsupported types and negative delays. NW4R
// SND converts delay * 5 to unsigned milliseconds. A negative delay therefore wraps to a huge one, and the LFO never
// starts.
PresetKey TrackPresetKey(const Track& tr)
{
    PresetKey pk;
    pk.program = tr.program;
    pk.attack = tr.attack <= 127 ? tr.attack : 0xff;
    pk.decay = tr.decay <= 127 ? tr.decay : 0xff;
    pk.sustain = tr.sustain <= 127 ? tr.sustain : 0xff;
    pk.release = tr.release <= 127 ? tr.release : 0xff;
    pk.hold = tr.hold <= 127 ? tr.hold : 0xff;
    if (tr.lfo_type <= 2 && tr.lfo_delay >= 0)
    {
        pk.lfo_type = tr.lfo_type;
        pk.lfo_range = tr.lfo_range;
        pk.lfo_speed = tr.lfo_speed;
        pk.lfo_delay = static_cast<uint16_t>(tr.lfo_delay);
    }
    else
    {
        pk.lfo_type = 3;
    }
    pk.init_pan = tr.init_pan;

    return pk;
}

// Copy pitch-related settings from track `tr` to channel `n`. SeqTrack::UpdateChannelParam does this every frame and
// before letting channels go.
void GiveTrackSettings(const Track& tr, Note& n)
{
    n.bend = tr.Bend();
    n.lfo.Set(tr.lfo_depth, tr.lfo_speed, tr.lfo_range, static_cast<uint32_t>(tr.lfo_delay * 5));
    n.lfo_type = tr.lfo_type;
}

// Build channel `n`'s current preset key from track `tr`. The key keeps the channel's original instrument and initial
// pan.
PresetKey ChannelPresetKey(const Track& tr, const Note& n)
{
    PresetKey pk = TrackPresetKey(tr);
    pk.program = n.program;
    pk.init_pan = n.init_pan;

    return pk;
}

// True if `a` and `b` have the same LFO settings.
bool SameLfo(const PresetKey& a, const PresetKey& b)
{
    return a.lfo_type == b.lfo_type && a.lfo_range == b.lfo_range && a.lfo_speed == b.lfo_speed &&
           a.lfo_delay == b.lfo_delay;
}

// `key` with the LFO settings of `lfo`.
PresetKey WithLfo(PresetKey key, const PresetKey& lfo)
{
    key.lfo_type = lfo.lfo_type;
    key.lfo_range = lfo.lfo_range;
    key.lfo_speed = lfo.lfo_speed;
    key.lfo_delay = lfo.lfo_delay;

    return key;
}

// Execute a sequence tick by tick, following NW4R SND's SeqPlayer, and record its output.
class Performer
{
public:
    // What MarkSteadyPass selected: whether the performance stops, the pass of the main loop it marked, if any, and
    // whether the performance plays one more pass to mark it.
    struct LoopChoice
    {
        bool stop = false;
        std::optional<std::size_t> marked;
        bool extend = false;
    };

    // Play as `build` does (SndBuild). `filter_controller` selects tracks with controller-driven filters (FilterMoves).
    // `start_velocities` gives the velocity to start MIDI notes at, by note id, from an earlier pass (Rises), and
    // `attacks` the attacks of MIDI notes the game releases during their attack (EarlyReleases). Multiply CC11 by
    // `level_scale` (LevelPeak). Give each track at most `part_limits` parts (Switch). Make the main loop's choices in
    // `loop_choices`, from an earlier pass (LoopChoices). Select by ChooseSteadyPass after them.
    Performer(const SoundInfo& sound, const Sequence& seq, BankSet& banks, const PerformOptions& options,
              const SndBuild& build, const std::array<bool, 16>& filter_controller,
              const std::map<uint32_t, int>& start_velocities, const std::map<uint32_t, int16_t>& attacks,
              double level_scale, const std::array<uint8_t, 16>& part_limits,
              const std::vector<LoopChoice>& loop_choices)
        : sound_(sound),
          seq_(seq),
          banks_(banks),
          options_(options),
          build_(build),
          balance_(!build.ax_2006 && sound.pan_mode == 1),
          rng_(options.seed),
          level_scale_(level_scale),
          filter_controller_(filter_controller),
          start_velocities_(start_velocities),
          attacks_(attacks),
          part_limits_(part_limits),
          replay_(loop_choices)
    {
        player_vars_.fill(-1);
        for (const auto& [index, value] : options.variables)
        {
            if (index < 16)
            {
                player_vars_[index] = value;
            }
            else if (index < 32)
            {
                global_vars_[index - 16] = value;
            }
        }
        out_.since_2008 = build.Since2008();
        out_.balance = balance_;
    }

    // Run the sound and return its recorded performance.
    Performance Run();

    // Tracks whose filters changed during a sounding note. Their filters require MIDI controllers because presets
    // cannot update an existing note.
    const std::array<bool, 16>& FilterMoves() const
    {
        return filter_moves_;
    }

    // Highest sounding CC11 value before `level_scale` and MIDI clamping. Game volumes can reach 255. This value may
    // therefore exceed 127.
    double LevelPeak() const
    {
        return level_peak_;
    }

    // The MIDI notes that tie or mono legatos raise above the velocity they started at, by note id, with the highest
    // velocity each reaches. A later pass starts them there (MidiNoteOn).
    const std::map<uint32_t, int>& Rises() const
    {
        return rises_;
    }

    // The parts each track used: one more than the highest.
    const std::array<uint8_t, 16>& PartsUsed() const
    {
        return parts_used_;
    }

    // The MIDI notes the game releases before their SoundFont attack ends, more than 1 dB quieter than the attack has
    // reached then, by note id. Each value is the attack in timecents that reaches the game's level at the release. A
    // later pass gives the notes those attacks (MidiNoteOn).
    const std::map<uint32_t, int16_t>& EarlyReleases() const
    {
        return early_releases_;
    }

    // The choices MarkSteadyPass made for the main loop, for a later pass to make again.
    const std::vector<LoopChoice>& LoopChoices() const
    {
        return choices_;
    }

private:
    // When a tick comes: the sound frame it's processed in, the milliseconds left in that frame after it, and its time.
    // The build of October 2006 counts time in 416ths of a tick instead (NextTick). There `units` is that count after
    // the frame takes out its ticks, `ticks_left` the number of the frame's ticks after this one, and `nominal_ms` the
    // tick's time at MIDI's tempo.
    struct TickTime
    {
        uint64_t frame = 0;
        float rest = 0.0f;
        double ms = 0;
        uint16_t units = 0;
        int ticks_left = 0;
        double nominal_ms = 0;
    };

    // When a track last ran a command: the tick, and the number of commands all tracks had run before it. The number
    // orders the commands of a tick.
    struct CommandRun
    {
        uint32_t tick = 0;
        uint64_t order = 0;
    };

    // Sound frame containing `ms`.
    static uint64_t FrameOf(double ms)
    {
        return static_cast<uint64_t>(ms / kFrameMs + 1e-9);
    }

    // Record `now_ms_` for the new `tick_`, and its frame start as a fractional tick (Performance::frame_ticks).
    void RecordTickTime()
    {
        tick_ms_.push_back(now_ms_);

        // The build of October 2006 runs a tick at the start of its frame. MIDI's tempo puts the tick at `nominal_ms_`,
        // up to a frame earlier.
        if (build_.ax_2006)
        {
            out_.frame_ticks.push_back(static_cast<double>(tick_) + (now_ms_ - nominal_ms_) / MidiMsPerTick());
            return;
        }

        const double start = static_cast<double>(frame_) * kFrameMs;
        std::size_t k = tick_ms_.size() - 1;
        while (k > 0 && tick_ms_[k] > start)
        {
            k--;
        }

        double tick = static_cast<double>(k);
        if (k + 1 < tick_ms_.size() && tick_ms_[k + 1] > tick_ms_[k])
        {
            tick += (start - tick_ms_[k]) / (tick_ms_[k + 1] - tick_ms_[k]);
        }
        out_.frame_ticks.push_back(std::min(tick, static_cast<double>(tick_)));
    }

    // Ticks per millisecond at the current tempo (SeqPlayer::GetBaseTempo), in the game's single precision. 0 at tempo
    // 0 or timebase 0. Either stops sequence timing.
    float TicksPerMs() const
    {
        return 1.0f * static_cast<float>(timebase_ * tempo_) / 60000.0f;
    }

    // When the next tick comes at the current tempo. Nothing when tempo or timebase is 0: NW4R SND's sequence timing
    // stops then.
    std::optional<TickTime> NextTick() const;

    // After sequence timing stops, continue sounding channels at the MIDI tempo (EmitTempo). Releases and one-shot
    // waves finish normally; held notes last kHoldSeconds. No further sequence commands run.
    void PlayOutStopped();

    // Real milliseconds per tick at the current tempo, or 0 at tempo 0 or timebase 0. A tick of the build of October
    // 2006 lasts 416 / tempo frames, whatever the timebase.
    double MsPerTick() const
    {
        if (build_.ax_2006)
        {
            return tempo_ > 0 ? kFrameMs * kTickUnits / tempo_ : 0.0;
        }

        const double tpm = static_cast<double>(timebase_) * tempo_; // ticks per minute
        return tpm > 0 ? 60000.0 / tpm : 0.0;
    }

    // Milliseconds per tick at MIDI's tempo (EmitTempo).
    double MidiMsPerTick() const
    {
        return 60000.0 / (out_.tempo.back().bpm * out_.timebase);
    }

    // First tick at or after `ms`. Extrapolate beyond the last tick at the final tempo, or the MIDI tempo if sequence
    // timing stopped.
    uint32_t MsToTick(double ms) const;

    // Record the current tempo. At tempo or timebase 0, the sequence stops advancing (Run). MIDI keeps its previous
    // tempo. The build of October 2006 runs tempo / 416 ticks a frame (NextTick).
    void EmitTempo()
    {
        const double bpm = build_.ax_2006
                               ? static_cast<double>(tempo_) * (60000.0 / kFrameMs / kTickUnits) / out_.timebase
                               : static_cast<double>(tempo_) * timebase_ / out_.timebase;
        if (bpm == 0.0)
        {
            return;
        }

        if (!out_.tempo.empty() && out_.tempo.back().tick == tick_)
        {
            out_.tempo.back().bpm = bpm;
        }
        else if (out_.tempo.empty() || out_.tempo.back().bpm != bpm)
        {
            out_.tempo.push_back({tick_, bpm});
        }
    }

    // Record an event for track `t` at the current tick, on `part` or else the part the track plays on, and update
    // Track::last_change.
    //
    // NW4R SND applies track settings at frame end (SeqTrack::UpdateChannelParam). A note uses the settings from the
    // end of its tick, changes after its note command included. Insert controller changes before that tick's
    // notes/presets. Each change replaces any earlier value for the same setting.
    void Emit(int t, TrackEvent e, int part = -1)
    {
        e.tick = tick_;
        e.part = static_cast<uint8_t>(part >= 0 ? part : tracks_[t].part);
        if (e.kind != EventKind::kExpression && e.kind != EventKind::kFilter && e.kind != EventKind::kPitch)
        {
            tracks_[t].last_change = tick_; // UpdateOutputs handles the three values that can differ between passes
        }

        std::vector<TrackEvent>& events = out_.tracks[t];
        if (!IsTrackSetting(e.kind))
        {
            events.push_back(e);
            return;
        }

        auto at = events.end();
        auto at_tick = [&](std::vector<TrackEvent>::iterator it)
        {
            return it != events.begin() && std::prev(it)->tick == tick_;
        };
        while (at_tick(at) && !IsTrackSetting(std::prev(at)->kind) && std::prev(at)->kind != EventKind::kSoundOff)
        {
            --at;
        }
        for (auto it = at; at_tick(it) && IsTrackSetting(std::prev(it)->kind); --it)
        {
            if (std::prev(it)->kind == e.kind && std::prev(it)->part == e.part)
            {
                *std::prev(it) = e;
                GiveTheFrameTheBiquad(t, e);
                return;
            }
        }
        events.insert(at, e);
        GiveTheFrameTheBiquad(t, e);
    }

    // The SFZ files select a filtered or an unfiltered region at note-on, from the biquad's controller (sfz.cpp). The
    // game gives the notes that a sound frame starts the settings that the frame ends with. When a tick switches the
    // biquad on or off, each note that an earlier tick of its frame started on the part gets a copy of the controller
    // before it. The copies go before notes of earlier ticks only. That keeps the events of a loop that starts in the
    // frame in place.
    void GiveTheFrameTheBiquad(int t, const TrackEvent& e)
    {
        // The ticks played out after a sequence stops advancing have no recorded time (PlayOutStopped). No command
        // switches the biquad then.
        if (e.kind != EventKind::kSfzBiquad || tick_ >= tick_ms_.size())
        {
            return;
        }

        // Find the events of the frame's earlier ticks, just before the current tick's events.
        std::vector<TrackEvent>& events = out_.tracks[t];
        std::size_t end = events.size();
        while (end > 0 && events[end - 1].tick == tick_)
        {
            end--;
        }

        const uint64_t frame = FrameOf(tick_ms_[tick_]);
        std::size_t begin = end;
        while (begin > 0 && events[begin - 1].after == 0 && FrameOf(tick_ms_[events[begin - 1].tick]) == frame)
        {
            begin--;
        }

        // The controller's value before the frame, 0 (off) if it was never sent.
        int value = 0;
        for (std::size_t k = begin; k > 0; k--)
        {
            if (events[k - 1].kind == EventKind::kSfzBiquad && events[k - 1].part == e.part)
            {
                value = events[k - 1].value;
                break;
            }
        }

        // Each of those notes on the part that would get the other choice gets a copy of the controller first.
        for (std::size_t k = begin; k < end; k++)
        {
            const TrackEvent& ev = events[k];
            if (ev.part != e.part)
            {
                continue;
            }

            if (ev.kind == EventKind::kSfzBiquad)
            {
                value = ev.value;
            }
            else if (ev.kind == EventKind::kNoteOn && (value != 0) != (e.value != 0))
            {
                TrackEvent copy = e;
                copy.tick = ev.tick;
                events.insert(events.begin() + static_cast<std::ptrdiff_t>(k), copy);
                k++;
                end++;
                value = e.value;
            }
        }
    }

    // Count an approximation required by MIDI or SoundFont.
    void Approximate(const char* what)
    {
        out_.approximations[what]++;
    }

    // Include a stopped note's end time in the sound's duration.
    void Ended(const Note& n)
    {
        sound_end_ms_ = std::max(sound_end_ms_, std::min(n.SoundEndMs(), now_ms_));
    }

    // True if `n` hasn't been released and its envelope has sunk below hearing (Note::quiet_ms). Such a note then
    // counts as silence when looking for loops, held sounds and the silence at the time limit.
    bool BelowHearing(const Note& n) const
    {
        return !n.released && now_ms_ >= n.quiet_ms;
    }

    // Allocate the tracks that the sound's flags select (SeqPlayer::Setup, main.dol 0x8003e1f4).
    void AllocateTracks();

    // End all notes and record duration/end tick. `finished` means the sequence ended naturally.
    void EndPerformance(bool finished);

    // Starts track `t` at command offset `offset`.
    void OpenTrack(int t, uint32_t offset);

    // Releases track `t`'s channels and closes the track.
    void CloseTrack(int t);

    // Run open track `t` for one tick. Return true when it reaches FIN or is stopped for running without waits.
    bool ParseNextTick(int t);

    // Like ParseNextTick after the tick's updates, run track `t`'s commands until it waits.
    bool ParseUntilWait(int t);

    // Start a tick: the random generator and the one-shot waves catch up with its sound frame.
    void StartTick();

    // Run the open tracks from track `first` on for one tick, and close those that finish. Return true if one of them
    // is still open.
    bool ParseTracks(int first);

    // End a tick: send each track's changes, and report the notes whose LFOs play otherwise than their presets.
    void FinishTick();

    // Move on to the tick at `next`. Send the pitches that timed sweeps reach on the way.
    void AdvanceTo(const TickTime& next);

    // A setting of a track's part: its track, part and kind of event.
    using SettingKey = std::tuple<int, uint8_t, EventKind>;

    // How a pass of the main loop starts: each track part's settings and the tempo after the commands of its first
    // tick, and the part each track's new notes go to.
    struct PassStart
    {
        std::map<SettingKey, TrackEvent> settings;
        double bpm = 120.0;
        std::array<uint8_t, 16> parts{};
    };

    // The settings of each track's parts after the commands of `tick`, from `perf`'s events.
    static std::map<SettingKey, TrackEvent> SettingsAt(const Performance& perf, uint32_t tick);

    // How a pass that starts at `tick` starts.
    PassStart PassStartAt(uint32_t tick) const;

    // A copy of this performer that plays on from the tick in which track `t` jumped back to `target` and stopped the
    // performance, up to but not including tick `until`, with no limit on the loops.
    Performer PlayOn(int t, uint32_t target, uint32_t until) const;

    // How the next pass of the main loop starts when track `t` has jumped back to `target` in this tick.
    PassStart NextPassStart(int t, uint32_t target) const;

    // True if events `a` and `b` give a setting the same value.
    static bool SameSetting(const TrackEvent& a, const TrackEvent& b);

    // True if a player that loops on the pass from `start` to `end` plays each repeat's notes as the game plays the
    // notes of the next pass. This performer has played that pass. Each repeat must have the same notes at the same
    // ticks, with the same settings and tempo. The player keeps the loop end's values, except for those that the pass
    // sends before a note and those that RestoreAtLoopStart sends again at the loop's start. The notes of a pass and of
    // the next may go to different parts of a track, each with the track's settings.
    bool RepeatsAsTheGame(uint32_t start, uint32_t end) const;

    // Select the first of the loop owner's passes that plays as the pass after it does (RepeatsAsTheGame). A player
    // that loops on the markers plays each repeat as the marked pass. Track `t` has just jumped back to `target`.
    LoopChoice ChooseSteadyPass(int t, uint32_t target) const;

    // Mark the pass that ChooseSteadyPass selects. Return true if the performance can stop, false if it requires
    // another pass first.
    bool MarkSteadyPass(int t, uint32_t target);

    // Send at the main loop's start each setting, and the tempo, that moves during the loop and that the game gives
    // back its value at the loop's start in `next_pass`. A player that loops on the markers otherwise keeps the loop
    // end's value. MIDI only sends a value when it changes.
    void RestoreAtLoopStart(const PassStart& next_pass);

    // Read and execute one command on track `t`. Return true for FIN.
    bool Parse(int t);

    // Read the next byte on track `t`, or return 0xff (FIN) at end of data.
    uint8_t ReadByte(int t);

    // Reads an argument of track `t`, encoded as `type`.
    int32_t ReadArg(int t, ArgType type);

    // Resolve player, global or track variable `index` for track `t`. Return nullptr above 47.
    int16_t* Variable(int t, uint8_t index);

    // Execute a command with its arguments already decoded.
    void CommandProc(int t, uint32_t cmd, int32_t arg1, int32_t arg2);

    // The argument of a pan command. The build of October 2006 raises a pan of 0 or 1 by one (0x800e417c in Smooth
    // Moves' main.dol), for the pan, the initial pan and the surround pan.
    int32_t PanArgument(int32_t value) const
    {
        return build_.ax_2006 && value <= 1 ? value + 1 : value;
    }

    // AX's voice count: the most new channels, or legatos, that a track can play in one tick (NoteOn).
    int Voices() const
    {
        return build_.ax_2006 ? kVoicesOctober2006 : kVoices;
    }

    // Execute 0xf0 arithmetic (0x80-0x8b) or comparison (0x90-0x95) on variable `index`. Comparisons set the flag used
    // by the if prefix.
    void VariableCommand(int t, uint32_t op, uint8_t index, int16_t v);

    // Record that track `t` reads player or global variable `index` (a comparison or an argument), or changes it. A
    // change sets Track::last_signal if another open track has read the variable and can still read on.
    void NoteVariableRead(int t, uint8_t index);
    void NoteVariableChange(int t, uint8_t index);

    // Unmutes track `t` (mode 0) or mutes it (1-3), first releasing (2) or stopping (3) its channels.
    void Mute(int t, uint8_t mode);

    // Ends a loop of track `t`: jumps back to its start until its count runs out.
    void LoopEnd(int t);

    // Handle track `t` jumping back to `target`. Detect the main loop and stop after the requested repetitions.
    void OnLoop(int t, uint32_t target);

    // Play a note on track `t`. Allocate a channel for it, or reuse the newest one for tie/mono.
    void NoteOn(int t, int key, int velocity, int32_t length);

    // Move channel `n` to `key` without another attack. Continue its MIDI note where possible, once this tick has
    // reached the kVoices legato cap (`past_cap`; see NoteOn), and on a channel below hearing (BelowHearing). Otherwise
    // start a new one. A legato doesn't start the envelope again. A channel below hearing therefore stays there, and
    // its legato plays nothing.
    void Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap);

    // Start `n`'s sweep. The sweep combines track sweep pitch and portamento from the previous key.
    void StartSweep(int t, Note& n, int key, int32_t length);

    // Start/end channel `n`'s MIDI note. The note plays `region` (null if none plays its key and velocity). Multiply
    // track CC11 by `gain` while the note plays (see Legato).
    void MidiNoteOn(int t, Note& n, int velocity, const Region* region, double gain = 1.0);
    void MidiNoteOff(int t, Note& n);

    // End `n`'s MIDI note on release, stop or replacement. Remove unheard notes entirely (see Unheard).
    void EndMidiNote(int t, Note& n);

    // True before `n` has been heard. Envelopes start at -90.4 dB and first advance at the end of the note-on frame.
    bool Unheard(const Note& n) const;

    // True if released channel `n` remains above the inaudibility threshold (kInaudibleSustain).
    bool StillAudible(const Note& n) const;

    // True if channel `n` can be heard: if it's released, above the inaudibility threshold (StillAudible).
    bool Heard(const Note& n) const;

    // True if no channel of track `t` but `n` can be heard on the part the track plays on, released ones included. CC11
    // acts on the whole MIDI channel. Only a note that's alone can therefore start higher and be brought down to its
    // velocity by CC11 (MidiNoteOn).
    bool Alone(int t, const Note& n) const;

    // True if a channel of track `t` on `part`, other than `except`, still sounds.
    bool PartSounds(int t, uint8_t part, const Note* except = nullptr) const;

    // Move track `t`'s new notes and changes to a part on which nothing but `except` sounds, within its part limit. The
    // notes on the track's current part keep the settings they have. Return false if every part sounds.
    bool Switch(int t, const Note* except = nullptr);

    // Like tie, mono and mute mode 2, release track `t`'s channels and detach them. Give them the track's settings
    // first.
    void ReleaseAndDetach(int t);

    // After track `t` detaches its channels (DetachAll), move the track to another part if they still sound on its
    // part. In the game they keep their settings. On a shared MIDI channel, the track's later changes would reach them.
    void LeaveTails(int t);

    // Releases channel `n` (Channel::Release, main.dol 0x8003809c): a channel already releasing goes on as it is.
    void ReleaseNote(int t, Note& n);

    // Record the attack for released channel `n` if the game releases its MIDI note before the SoundFont attack ends,
    // more than 1 dB quieter than that attack has reached (EarlyReleases).
    void RecordEarlyRelease(const Note& n);

    // Stops channel `n` at once.
    void StopNote(int t, Note& n);

    // Detach track `t`'s channels. They keep sounding.
    void DetachAll(int t);

    // Remove stopped channels from track `t`, including detached channels.
    void PruneNotes(int t);

    // Advance track `t`'s channels by one tick. Release notes whose lengths expire and remove stopped channels.
    void UpdateNoteLengths(int t);

    // Run Channel::Update for one-shot waves from the previous tick's frame up to, but excluding, `frame`. Compute each
    // next frame's pitch from key, sweep, track bend and pitch LFO.
    void UpdateOneShots(uint64_t frame);

    // Emit changed volume, pan, pitch and other controllers for track `t`.
    void UpdateOutputs(int t);

    // The pitch MIDI gives part `part` of track `t` in sound frame `frame`, in semitones. On the part the track plays
    // on, that's the track's bend and legato key offset with the newest sweep of its notes there. A part that the track
    // has left follows the newest note there that can be heard: that note's bend, the offset of its key from the key of
    // its MIDI note, and the newest sweep on the part. Nothing if the part has no such note.
    std::optional<float> PartPitch(int t, uint8_t part, uint64_t frame) const;

    // Send part `part` of track `t` its pitch in sound frame `frame` if that has changed: at the current tick, or for a
    // frame between ticks, `after` it by a fraction of a tick (TrackEvent::after). Return true if it was sent.
    bool SendPitch(int t, uint8_t part, uint64_t frame, double after = 0);

    // True if notes that track `t` has detached still sound on the part that the track plays on. That part takes the
    // track's later changes. No other part was free for the track (LeaveTails).
    bool TailsOnPart(int t) const;

    // Send the pitches that timed sweeps reach in the sound frames between the current tick's frame and `next`'s frame.
    void SweepBetweenTicks(const TickTime& next);

    // True if a note of track `t` on `part` that sounds at `ms` has a timed sweep still moving after sound frame
    // `frame`.
    bool SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const;

    // Send the pitches that the timed sweeps of a finished sequence's release tails reach after its last tick. Each
    // pitch goes as far after that tick as its frame starts, at the sequence's last tempo.
    void SweepAfterEnd();

    // Return track `t`'s CC11 before legato gain, `level_scale_` and the 127 limit (UpdateOutputs).
    double TrackLevel(int t) const;

    // Return the volume NW4R SND gives track `t`'s channels (SeqTrack::UpdateChannelParam): each volume squared, and
    // the sound's volume, clamped to 1 (SeqPlayer's volume).
    float UserVolume(int t) const;

    // Report the notes of track `t` that play quieter than in the game because the track's volumes are above full level
    // (TrackLevel).
    void CheckFullLevel(int t);

    // Find `key` in the preset list. Add it if it's absent.
    int PresetIndex(const PresetKey& key);

    // Build channel `n`'s current preset key. Use a controller for the filter if track `t` is in `filter_controller_`.
    PresetKey NoteKey(int t, const Note& n) const;

    // Recalculate track `t`'s filter and record changes affecting sounding notes (FilterMoves). As in the game,
    // detached channels keep their previous filter.
    void SetFilter(int t);

    // Set track `t`'s LFO `setting` to `value`. The game gives a track's sounding notes the track's LFO settings at the
    // end of each sound frame (SeqTrack::UpdateChannelParam). A preset fixes them at note-on. The notes struck earlier
    // in this tick get the new settings (GiveStruckNotesLfo). CheckLfo reports later changes.
    template <typename T>
    void SetLfoSetting(int t, T& setting, T value);

    // Give the notes that track `t` struck in this tick presets with the track's LFO settings. This tick's preset
    // events are sent again in front of its notes.
    void GiveStruckNotesLfo(int t);

    // At the end of a tick, report each of track `t`'s notes whose LFO plays with other settings than its preset has.
    void CheckLfo(int t);

    // Returns true if one of track `t`'s channels is still sounding.
    bool TrackBusy(int t) const;

    // True if any channel of track `t` except `except` is sounding, including detached channels.
    bool OthersSounding(int t, const Note* except) const;

    // True if open track `t` will never read on, or not before the game stops the sound: it waits a negative number of
    // ticks, waits out a note of kHeldTicks or more, or waits for held, looping notes that never end.
    bool WaitsForever(int t) const;

    // True if track `t` sounds nothing, and kHoldSeconds have passed since its last change or since it opened,
    // whichever is later.
    bool Quiet(int t) const;

    // True if no open track will read on before the game stops the sound and no note sounds: the sequence has ended.
    bool Stuck() const;

    // True if every open track is blocked on endless or held notes and no sweep, volume or pan can change.
    bool Steady() const;

    // Returns the time to end the performance at if the main loop holds a sound unchanged. The loop's pass started at
    // tick `start`. Such a pass is longer than kHoldSeconds and has changed nothing for longer than that. The pass
    // sounds only endless notes, each the newest on an open track with tie or mono on. The next pass usually continues
    // them. The sound then holds forever. As with Steady, the performance ends kHoldSeconds after the last change.
    std::optional<double> HeldSoundEnd(uint32_t start) const;

    // True if the marked main loop sounds nothing once the notes before it have ended. No note starts from the loop's
    // start on. No channel has a MIDI note any more. Each channel has ended or ends without the game. Release tails and
    // fades may still play. The game keeps such a sound in silence until it stops the sound, or until a poll leaves the
    // loop.
    bool SilentLoop() const;

    const SoundInfo& sound_;
    const Sequence& seq_;
    BankSet& banks_;
    PerformOptions options_;
    SndBuild build_;
    bool balance_ = false; // stereo waves play in balance mode (Performance::balance)
    Random rng_;
    Performance out_;

    std::array<Track, 16> tracks_{};
    std::array<int16_t, 16> player_vars_{};
    std::array<int16_t, 16> global_vars_{}; // 0 until the game first sets one (SeqPlayer::SetGlobalVariable)
    uint32_t timebase_ = 48;
    int tempo_ = 120;
    uint8_t main_volume_ = 127;
    uint32_t tick_ = 0;
    double now_ms_ = 0;
    uint64_t frame_ = 0; // the sound frame the current tick is processed in
    float rest_ = 3.0f;  // milliseconds left in that frame after the tick (SeqPlayer::UpdateTick)

    // The build of October 2006's tick count (NextTick): time in 416ths of a tick, the ticks of the current frame after
    // this one, and the tick's time at MIDI's tempo.
    uint16_t tick_units_ = kTickUnits;
    int frame_ticks_left_ = 0;
    double nominal_ms_ = 0;

    std::array<std::map<uint32_t, CommandRun>, 16> last_run_; // when each command offset last ran, per track
    uint64_t commands_run_ = 0;                               // by all tracks
    std::vector<double> tick_ms_;                             // the time of each tick
    double sound_end_ms_ = 0;                                 // end time of the last completed note
    uint32_t next_midi_id_ = 1;
    uint64_t rng_frame_ = 0;  // sound frames the random generator has been advanced for
    uint64_t next_frame_ = 0; // first sound frame with pending channel updates
    bool stop_ = false;
    int stopped_by_ = -1;                     // the track whose pass of the main loop stopped the performance
    std::optional<double> held_sound_end_ms_; // set when the main loop holds a sound (HeldSoundEnd)
    bool silent_loop_ = false;                // the main loop sounded nothing (SilentLoop)
    std::optional<std::pair<int, uint32_t>> loop_owner_;      // the track and loop start of the marked loop (OnLoop)
    std::vector<std::pair<uint32_t, uint32_t>> owner_passes_; // the owner's counted passes, start and end ticks
    std::size_t marked_ = 0;                                  // the marked one of them (MarkSteadyPass)
    bool extended_ = false;             // the performance plays one more pass to mark it (ChooseSteadyPass)
    std::optional<uint32_t> last_draw_; // the last tick a track drew a random number in
    std::vector<std::tuple<uint32_t, int, uint8_t>> part_switches_; // tick, track and new part of each Switch
    std::array<uint16_t, 32> variable_readers_{}; // the tracks that have read each player or global variable
    double level_scale_ = 1.0;
    double level_peak_ = 0.0;
    std::array<bool, 16> filter_controller_{};
    std::array<bool, 16> filter_moves_{};
    std::map<uint32_t, int> start_velocities_; // from an earlier pass's Rises
    std::map<uint32_t, int> rises_;
    std::map<uint32_t, int16_t> attacks_; // from an earlier pass's EarlyReleases
    std::map<uint32_t, int16_t> early_releases_;
    std::array<uint8_t, 16> part_limits_{};
    std::vector<LoopChoice> replay_;  // the choices of an earlier pass to make again (MarkSteadyPass)
    std::size_t replayed_ = 0;        // how many of them have been made
    std::vector<LoopChoice> choices_; // the choices made
    std::array<uint8_t, 16> parts_used_{};
};

int16_t* Performer::Variable(int t, uint8_t index)
{
    // Player variables 0-15, global 16-31 (SeqPlayer::GetVariablePtr, main.dol 0x8003ed7c), track 32-47.
    if (index < 16)
    {
        return &player_vars_[index];
    }
    if (index < 32)
    {
        return &global_vars_[index - 16];
    }
    if (index < 48)
    {
        return &tracks_[t].vars[index - 32];
    }

    return nullptr;
}

uint8_t Performer::ReadByte(int t)
{
    Track& tr = tracks_[t];
    if (tr.current >= seq_.data.size())
    {
        tr.current = static_cast<uint32_t>(seq_.data.size()) + 1;
        return 0xff; // past the end: behave like FIN
    }

    return seq_.data[tr.current++];
}

int32_t Performer::ReadArg(int t, ArgType type)
{
    // MmlParser::ReadArg (main.dol 0x8003c638).
    switch (type)
    {
    case kArgU8:
        return ReadByte(t);

    case kArgS16:
        {
            const uint32_t hi = ReadByte(t);
            const uint32_t lo = ReadByte(t);
            return static_cast<int32_t>(((hi << 8) & 0xffff) | lo);
        }

    case kArgVmidi:
        {
            // Accept any encoded length. ReadByte returns 0xff at end of data. That ends the number there.
            uint32_t v = 0;
            uint8_t b;
            do
            {
                b = ReadByte(t);
                v = (b & 0x7f) | (v << 7);
            } while ((b & 0x80) && tracks_[t].current <= seq_.data.size());
            return static_cast<int32_t>(v);
        }

    case kArgRandom:
        {
            const uint32_t h1 = ReadByte(t), l1 = ReadByte(t);
            const int32_t lo = static_cast<int16_t>(((h1 << 8) & 0xffff) | l1);
            const uint32_t h2 = ReadByte(t), l2 = ReadByte(t);
            const int32_t hi = static_cast<int16_t>(((h2 << 8) & 0xffff) | l2);
            const uint32_t r = rng_.Next();
            last_draw_ = tick_;

            // Like the PowerPC code: 32-bit wrapping multiply and arithmetic shift.
            const int32_t prod = static_cast<int32_t>(static_cast<uint32_t>((hi - lo) + 1) * r);
            return lo + (prod >> 16);
        }

    case kArgVariable:
        {
            const uint8_t index = ReadByte(t);
            const int16_t* v = Variable(t, index);
            NoteVariableRead(t, index);
            return v ? *v : 0;
        }

    default:
        return 0;
    }
}

int Performer::PresetIndex(const PresetKey& key)
{
    for (std::size_t i = 0; i < out_.presets.size(); i++)
    {
        if (out_.presets[i] == key)
        {
            return static_cast<int>(i);
        }
    }

    out_.presets.push_back(key);

    return static_cast<int>(out_.presets.size() - 1);
}

PresetKey Performer::NoteKey(int t, const Note& n) const
{
    PresetKey pk = ChannelPresetKey(tracks_[t], n);
    pk.attack_timecents = n.attack_timecents;
    if (filter_controller_[t])
    {
        pk.filter_controller = true;
    }
    else
    {
        pk.filter = tracks_[t].filter;
    }

    return pk;
}

void Performer::SetFilter(int t)
{
    Track& tr = tracks_[t];
    const SoundFontFilter filter = ToSoundFont(tr.low_pass, tr.biquad_type, tr.biquad_value);
    if (filter != tr.filter && TrackBusy(t))
    {
        filter_moves_[t] = true;
    }
    tr.filter = filter;
}

template <typename T>
void Performer::SetLfoSetting(int t, T& setting, T value)
{
    const PresetKey before = TrackPresetKey(tracks_[t]);
    setting = value;
    if (!SameLfo(TrackPresetKey(tracks_[t]), before))
    {
        GiveStruckNotesLfo(t);
    }
}

void Performer::GiveStruckNotesLfo(int t)
{
    // This tick's events, and the notes struck in them.
    Track& tr = tracks_[t];
    std::vector<TrackEvent>& events = out_.tracks[t];
    auto first = events.end();
    while (first != events.begin() && std::prev(first)->tick == tick_)
    {
        --first;
    }

    std::set<uint32_t> struck;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn)
        {
            struck.insert(it->note);
        }
    }
    if (struck.empty())
    {
        return;
    }

    // The preset each part had before this tick.
    std::map<uint8_t, int> sent;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kNoteOn && !sent.contains(it->part))
        {
            const uint8_t part = it->part;
            auto is_part_preset = [part](const TrackEvent& e)
            {
                return e.kind == EventKind::kPreset && e.part == part;
            };
            const auto back = std::find_if(std::make_reverse_iterator(first), events.rend(), is_part_preset);
            sent[part] = back != events.rend() ? static_cast<int>(back->preset) : -1;
        }
    }

    // Each note's preset with the track's LFO settings goes in front of it, in place of this tick's preset events.
    const PresetKey lfo = TrackPresetKey(tr);
    std::map<uint8_t, int> struck_with = sent;
    std::vector<TrackEvent> tick_events;
    for (auto it = first; it != events.end(); ++it)
    {
        if (it->kind == EventKind::kPreset)
        {
            struck_with[it->part] = static_cast<int>(it->preset);
            continue;
        }

        const int with = it->kind == EventKind::kNoteOn ? struck_with[it->part] : -1;
        if (with >= 0)
        {
            const int preset = PresetIndex(WithLfo(out_.presets[static_cast<std::size_t>(with)], lfo));
            if (preset != sent[it->part])
            {
                TrackEvent ev{tick_, EventKind::kPreset};
                ev.part = it->part;
                ev.preset = static_cast<uint32_t>(preset);
                tick_events.push_back(ev);
                sent[it->part] = preset;
            }
        }
        tick_events.push_back(*it);
    }
    events.erase(first, events.end());
    events.insert(events.end(), tick_events.begin(), tick_events.end());

    if (sent.contains(tr.part))
    {
        tr.out_preset = sent[tr.part];
    }
    for (Note& n : tr.notes)
    {
        if (struck.contains(n.midi_id))
        {
            n.preset = WithLfo(n.preset, lfo);
        }
    }
}

void Performer::CheckLfo(int t)
{
    Track& tr = tracks_[t];
    const PresetKey lfo = TrackPresetKey(tr);
    if (tr.lfo_depth == 0 || lfo.lfo_type > 2)
    {
        return;
    }

    for (Note& n : tr.notes)
    {
        if (n.midi_on && !n.lfo_moved && n.EndMs() > now_ms_ && !SameLfo(n.preset, lfo))
        {
            n.lfo_moved = true;
            Approximate("LFO parameters changed during a note (using its note-on settings)");
        }
    }
}

void Performer::OpenTrack(int t, uint32_t offset)
{
    // SeqTrack::SetSeqData (main.dol 0x8003ffbc) and Open (0x8003ffcc) set where the track reads and open the track.
    // They leave the track's other parameters as they were, a wait and a call stack among them.
    Track& tr = tracks_[t];
    tr.current = offset;
    tr.open = true;
    tr.opened = tick_;
}

void Performer::MidiNoteOff(int t, Note& n)
{
    if (!n.midi_on)
    {
        return;
    }

    n.midi_on = false;
    TrackEvent ev{0, EventKind::kNoteOff, n.midi_key};
    ev.note = n.midi_id;
    Emit(t, ev, n.part);
}

void Performer::ReleaseNote(int t, Note& n)
{
    // Envelopes advance 3 ms per frame, from the note-on frame on. A released channel stops at the start of the first
    // frame whose level is silent, after that frame's ticks.
    if (n.released)
    {
        return;
    }

    const int64_t frame = static_cast<int64_t>(frame_);
    n.released = true;
    n.released_frame = frame_;
    n.release_level = HeldLevel(n.env, frame - static_cast<int64_t>(n.on_frame));
    n.release_rate = DecayRate(n.env.release);
    n.release_end_ms = static_cast<double>(frame + ReleaseFrames(n.release_level, n.release_rate) + 1) * kFrameMs;
    RecordEarlyRelease(n);
    EndMidiNote(t, n);
}

void Performer::RecordEarlyRelease(const Note& n)
{
    // The SoundFont attack starts with the MIDI note and reaches full level after Sf2AttackMs. A linear ramp to full
    // level over T milliseconds reaches the game's level L dB after t milliseconds when T = t * 10^(-L / 20).
    const double held_ms = static_cast<double>(frame_ - n.midi_frame) * kFrameMs;
    const double ramp_ms = Sf2AttackMs(n.env);
    if (!n.midi_on || held_ms <= 0.0 || held_ms >= ramp_ms)
    {
        return;
    }

    const double level_db = n.release_level / 10.0;
    if (20.0 * std::log10(held_ms / ramp_ms) - level_db <= 1.0)
    {
        return;
    }

    const double attack_ms = held_ms * std::pow(10.0, -level_db / 20.0);
    early_releases_[n.midi_id] = static_cast<int16_t>(ToTimecents(attack_ms, -12000, 8000));
}

void Performer::EndMidiNote(int t, Note& n)
{
    if (!n.midi_on || !Unheard(n))
    {
        MidiNoteOff(t, n);
        return;
    }

    std::vector<TrackEvent>& events = out_.tracks[t];
    auto is_note_on = [&](const TrackEvent& e)
    {
        return e.kind == EventKind::kNoteOn && e.note == n.midi_id;
    };
    const auto on = std::find_if(events.rbegin(), events.rend(), is_note_on);
    if (on != events.rend())
    {
        events.erase(std::next(on).base());
    }

    n.midi_on = false;
}

bool Performer::Unheard(const Note& n) const
{
    return frame_ == n.on_frame;
}

bool Performer::StillAudible(const Note& n) const
{
    const double steps = static_cast<double>(frame_ - n.released_frame);
    const double level = n.release_level - n.release_rate * kEnvStepMs * steps;
    return n.EndMs() > now_ms_ && level > -kInaudibleSustain;
}

bool Performer::Heard(const Note& n) const
{
    return n.released ? StillAudible(n) : n.EndMs() > now_ms_;
}

bool Performer::Alone(int t, const Note& n) const
{
    const Track& tr = tracks_[t];
    auto other_heard = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.part == tr.part && Heard(other);
    };

    return std::ranges::none_of(tr.notes, other_heard) && std::ranges::none_of(tr.detached, other_heard);
}

bool Performer::PartSounds(int t, uint8_t part, const Note* except) const
{
    const Track& tr = tracks_[t];
    auto sounds = [&](const Note& n)
    {
        return &n != except && n.HasMidiNote() && n.part == part && n.EndMs() > now_ms_;
    };

    return std::ranges::any_of(tr.notes, sounds) || std::ranges::any_of(tr.detached, sounds);
}

bool Performer::Switch(int t, const Note* except)
{
    Track& tr = tracks_[t];
    for (uint8_t part = 0; part < part_limits_[t]; part++)
    {
        if (part == tr.part || PartSounds(t, part, except))
        {
            continue;
        }

        // The new part's channel may hold another part's values from earlier. Send every controller again.
        tr.left[tr.part] = {tr.out_volume,     tr.out_expression, tr.out_pan,    tr.out_mod,
                            tr.out_reverb,     tr.out_chorus,     tr.out_filter, tr.out_sfz_low_pass,
                            tr.out_sfz_biquad, tr.out_pitch,      tr.legato_gain};
        tr.part = part;
        part_switches_.emplace_back(tick_, t, part);
        tr.out_preset = -1;
        tr.out_volume = tr.out_expression = tr.out_pan = tr.out_mod = tr.out_reverb = tr.out_chorus = tr.out_filter =
            -1;
        tr.out_sfz_low_pass = tr.out_sfz_biquad = -1;
        tr.out_pitch = std::numeric_limits<float>::infinity();
        parts_used_[t] = std::max<uint8_t>(parts_used_[t], part + 1);
        return true;
    }

    return false;
}

void Performer::ReleaseAndDetach(int t)
{
    // SeqTrack::ReleaseAllChannel gives the channels the track's settings before it releases them. The end of a tick
    // does the same. So the MIDI part the notes sound on gets the tick's changes so far before the track leaves it.
    UpdateOutputs(t);
    for (Note& n : tracks_[t].notes)
    {
        ReleaseNote(t, n);
    }
    DetachAll(t);
    LeaveTails(t);
}

void Performer::LeaveTails(int t)
{
    const Track& tr = tracks_[t];
    auto tail = [&](const Note& n)
    {
        return n.HasMidiNote() && n.part == tr.part && StillAudible(n);
    };
    if (std::ranges::any_of(tr.detached, tail))
    {
        Switch(t);
    }
}

void Performer::DetachAll(int t)
{
    // FreeAllChannel (main.dol 0x800408c4). Copy the track's final settings before detaching.
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        GiveTrackSettings(tr, n);
        tr.detached.push_back(n);
    }
    tr.notes.clear();
}

bool Performer::OthersSounding(int t, const Note* except) const
{
    const Track& tr = tracks_[t];
    for (const Note& n : tr.notes)
    {
        if (&n != except && n.EndMs() > now_ms_ && !BelowHearing(n))
        {
            return true;
        }
    }

    for (const Note& n : tr.detached)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

void Performer::StopNote(int t, Note& n)
{
    // Channel::Stop (main.dol 0x80038104) silences immediately. MIDI CC120 cuts the whole channel. Use it only for an
    // audible note with no other sounds remaining on its part.
    const bool heard = n.HasMidiNote() && !Unheard(n);
    EndMidiNote(t, n);
    if (heard && n.EndMs() > now_ms_)
    {
        if (!PartSounds(t, n.part, &n))
        {
            Emit(t, {0, EventKind::kSoundOff}, n.part);
        }
        else
        {
            Approximate("immediate channel stop (MIDI release tail remains)");
        }
    }

    n.released = true;
    n.release_end_ms = now_ms_;
}

void Performer::CloseTrack(int t)
{
    // SeqTrack::Close (main.dol 0x8003ffd8) releases every channel, even one that ignores note-off, and detaches them.
    // ReleaseAllChannel first gives them the track's settings. The end of a tick does the same. The release tails then
    // start from the closing tick's volume, pan, pitch and filter.
    Track& tr = tracks_[t];
    if (TrackBusy(t))
    {
        UpdateOutputs(t);
        level_peak_ = std::max(level_peak_, TrackLevel(t) * tr.legato_gain);
    }

    for (Note& n : tr.notes)
    {
        ReleaseNote(t, n);
    }

    DetachAll(t);
    tr.open = false;
}

bool Performer::TrackBusy(int t) const
{
    for (const Note& n : tracks_[t].notes)
    {
        if (n.EndMs() > now_ms_)
        {
            return true;
        }
    }

    return false;
}

bool Performer::Quiet(int t) const
{
    const Track& tr = tracks_[t];
    const uint32_t since = std::max(tr.last_change.value_or(0), tr.opened);
    return !OthersSounding(t, nullptr) && now_ms_ - tick_ms_[since] > kHoldSeconds * 1000.0;
}

bool Performer::WaitsForever(int t) const
{
    // A track that waits a negative number of ticks never reads on. That happens to a note or wait whose length comes
    // from a variable left at -1. A track that waits out a held note reads on only after the game would have stopped
    // the sound.
    const Track& tr = tracks_[t];
    if (tr.wait < 0 || tr.held_wait)
    {
        return true;
    }
    if (!tr.note_finish_wait || tr.notes.empty())
    {
        return false;
    }

    for (const Note& n : tr.notes)
    {
        if (n.length > 0 || n.EndMs() < kNever)
        {
            return false;
        }
    }

    return true;
}

bool Performer::Steady() const
{
    bool any = false;
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        any = true;
        if (!WaitsForever(t) || tr.vol.Moving() || tr.pan.Moving())
        {
            return false;
        }
        for (const Note& n : tr.notes)
        {
            if (n.SweepAt(frame_) != 0.0f)
            {
                return false;
            }
        }
    }

    return any;
}

bool Performer::Stuck() const
{
    // A note that hasn't been released sounds here until it has faded (Note::faded_ms). The MIDI note of a tie ends at
    // the same point. The sequence's sound then ends where the note's MIDI note ends (EndPerformance).
    const auto sounding = [this](const Note& n)
    {
        return n.EndMs() > now_ms_ && (n.released || now_ms_ < n.faded_ms);
    };
    const auto ends_later = [this](const Note& n)
    {
        return n.EndMs() > now_ms_;
    };
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if ((tr.open && tr.wait >= 0 && !tr.held_wait) || std::ranges::any_of(tr.notes, sounding) ||
            std::ranges::any_of(tr.detached, ends_later))
        {
            return false;
        }
    }

    return true;
}

void Performer::PruneNotes(int t)
{
    // Remove channels when their one-shot wave or release finishes (SeqTrack::ChannelCallbackFunc, main.dol
    // 0x80040930).
    auto ended = [&](Note& n)
    {
        if (n.EndMs() > now_ms_)
        {
            return false;
        }

        MidiNoteOff(t, n); // the wave ended by itself
        Ended(n);

        return true;
    };
    std::erase_if(tracks_[t].notes, ended);
    std::erase_if(tracks_[t].detached, ended);
}

void Performer::UpdateNoteLengths(int t)
{
    // SeqTrack::UpdateChannelLength (main.dol 0x800400a8). From 2008, a region can ignore the note-off at the end of a
    // note's length (Mario Kart Wii's Channel::NoteOff, 0x80090600).
    Track& tr = tracks_[t];
    for (Note& n : tr.notes)
    {
        if (n.length > 0)
        {
            n.length--;
        }

        // Ignore-note-off regions continue playing. End their MIDI note on channel release/stop, wave completion or
        // inaudibility. In the game, silent channels keep their voices until another note takes them.
        if (n.length == 0 && !n.released && !tr.damper && !n.ignore_note_off)
        {
            ReleaseNote(t, n);
        }

        // The MIDI note of a channel that its length can't release, such as a tie, ends where the channel has faded to
        // kFadedAttenuation. The channel's sound ends there.
        if (!n.sunk && (n.ignore_note_off || n.length < 0) && !n.released && now_ms_ >= n.faded_ms)
        {
            MidiNoteOff(t, n);
            n.sunk = true;
            sound_end_ms_ = std::max(sound_end_ms_, n.faded_ms);
        }

        if (!n.sweep_in_ms)
        {
            n.sweep_ticks += 1.0;
        }
    }

    PruneNotes(t);
}

void Performer::UpdateOneShots(uint64_t frame)
{
    // NW4R SND updates channels after each frame's ticks, immediately after copying track settings. Track state stays
    // fixed between ticks. Each channel's intervening frames can therefore be processed together. Stop calculating
    // pitch once the wave ends.
    auto update = [&](Note& n)
    {
        if (!n.one_shot)
        {
            return;
        }

        for (uint64_t f = next_frame_; f < frame && !n.WaveEndedBy(f); f++)
        {
            const uint64_t per_frame = n.RatePerFrame(n.Cent(f));
            if (per_frame != n.per_frame || f + 1 >= n.start_frame)
            {
                n.ChangeRate(f, per_frame);
            }

            n.lfo.Update(kEnvStepMs);
        }
    };

    for (Track& tr : tracks_)
    {
        for (Note& n : tr.notes)
        {
            GiveTrackSettings(tr, n);
            update(n);
        }
        for (Note& n : tr.detached)
        {
            update(n);
        }
    }

    next_frame_ = std::max(next_frame_, frame);
}

float Performer::UserVolume(int t) const
{
    const Track& tr = tracks_[t];
    const float volume = static_cast<float>(tr.vol.Get()) / 127.0f;
    const float volume2 = static_cast<float>(tr.volume2) / 127.0f;
    const float main_volume = static_cast<float>(main_volume_) / 127.0f;
    float user = 1.0f;
    user *= volume * volume;
    user *= volume2 * volume2;
    user *= main_volume * main_volume;
    user *= std::clamp(static_cast<float>(sound_.volume) / 127.0f, 0.0f, 1.0f);

    return user;
}

void Performer::CheckFullLevel(int t)
{
    // With the track's volumes limited to full level (TrackLevel), a note whose velocity and region volume give less
    // than full level plays quieter than in the game. The game clamps only the product.
    Track& tr = tracks_[t];
    const float user = UserVolume(t);
    if (user <= 1.0f)
    {
        return;
    }

    for (Note& n : tr.notes)
    {
        if (!n.over_full_level && n.midi_on && n.init_volume < 1.0f)
        {
            n.over_full_level = true;
            Approximate("track volume above full level (limited to it; quieter notes play quieter than in the game)");
        }
    }
}

void Performer::UpdateOutputs(int t)
{
    // The values SeqTrack::UpdateChannelParam (main.dol 0x800403e0) gives the track's channels.
    Track& tr = tracks_[t];
    if (!tr.played)
    {
        return;
    }

    // A closed track's channels take nothing more from it, but go on with their timed sweeps (Channel::Update).
    if (!tr.open)
    {
        for (uint8_t part = 0; part < 16; part++)
        {
            if (frame_ > 0 && SweepMoves(t, part, frame_ - 1, now_ms_))
            {
                SendPitch(t, part, frame_);
            }
        }

        return;
    }

    // Detached notes keep their old settings in the game. In MIDI, those the track couldn't leave on another part
    // (LeaveTails) still share its channel and receive later controller changes.
    const bool tails = TailsOnPart(t);
    auto control = [&](int& last, int value, EventKind kind)
    {
        if (value != last)
        {
            last = value;
            Emit(t, {0, kind, 0, static_cast<uint8_t>(value)});
            if (tails)
            {
                Approximate("detached notes (still receive track controller changes in MIDI)");
            }
        }
    };

    // Channel gain is (vol * volume2 * main volume / 127^3)^2 * sound volume / 127 * (velocity / 127)^2. Each volume
    // but the sound's can reach 255. SoundFont CC7, CC11 and velocity all act squared: put up to 127 of track volume in
    // CC7, and the excess, other volumes and legato velocity change in CC11. If CC11 exceeds 127, rerun with all CC11
    // values scaled equally (Perform).
    control(tr.out_volume, std::clamp(tr.vol.Get(), 0, 127), EventKind::kVolume);
    const double level = TrackLevel(t);
    control(tr.out_expression, std::clamp(static_cast<int>(std::lround(level * tr.legato_gain * level_scale_)), 0, 127),
            EventKind::kExpression);
    const int change_filter = FilterControllerValue(tr.filter.cutoff);
    if (level != tr.change_level || change_filter != tr.change_filter)
    {
        if (level != tr.change_level)
        {
            CheckFullLevel(t);
        }

        tr.change_level = level;
        tr.change_filter = change_filter;
        tr.last_change = tick_;
    }

    // NW4R SND pan is value / 63, and the SoundFont CC10 modulator is (value - 64) / 64, on the synth's pan curve (see
    // pan.h).
    const int pan = std::clamp(static_cast<int>(std::lround(64.0 + SoundFontPan(tr.pan.Get() / 63.0) * 64.0)), 0, 127);
    auto stereo_on_part = [&](const Note& n)
    {
        return n.stereo && n.part == tr.part && n.EndMs() > now_ms_;
    };
    if (pan != tr.out_pan && pan != 64 && balance_ && std::ranges::any_of(tr.notes, stereo_on_part))
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }
    control(tr.out_pan, pan, EventKind::kPan);

    // A volume LFO changes a channel's level by CalcVolumeRatio(6 * its value) dB. That can't rise more than 6 dB. The
    // SoundFont and SFZ LFOs swing as far up as down. A deeper LFO therefore keeps to a 6 dB swing and dips less than
    // in the game. The game's LFO value reaches depth / 128 * range. Both outputs swing 12 * range * CC1 / 128 dB. A 6
    // dB swing takes CC1 = 64 / range.
    int mod = std::min(127, (tr.lfo_depth + 1) / 2);
    if (tr.lfo_type == 1 && tr.lfo_depth * tr.lfo_range > 128)
    {
        mod = std::min(mod, static_cast<int>(64.0 / tr.lfo_range));
    }
    control(tr.out_mod, mod, EventKind::kModulation);
    control(tr.out_reverb, std::min<int>(127, tr.fx_send_a), EventKind::kReverb);
    control(tr.out_chorus, std::min<int>(127, tr.fx_send_b), EventKind::kChorus);
    if (filter_controller_[t])
    {
        control(tr.out_filter, FilterControllerValue(tr.filter.cutoff), EventKind::kFilter);
    }

    // SFZ controllers hold the original filter settings. Each track file uses its first enabled biquad type; omit later
    // types.
    control(tr.out_sfz_low_pass, SfzLowPassValue(tr.low_pass), EventKind::kSfzLowPass);
    uint8_t biquad = SfzBiquadValue(tr.biquad_type, tr.biquad_value);
    if (biquad != 0 && out_.biquad_types[t] == 0)
    {
        out_.biquad_types[t] = static_cast<uint8_t>(tr.biquad_type);
    }
    if (tr.biquad_type != out_.biquad_types[t])
    {
        biquad = 0;
    }
    control(tr.out_sfz_biquad, biquad, EventKind::kSfzBiquad);

    // As in the game, a part that the track has left but whose notes it still holds follows the track's changes. The
    // part takes them at the level its notes have (their legato gain). A part that holds only notes that the track
    // detached keeps their settings.
    for (uint8_t part = 0; part < 16; part++)
    {
        auto held = [&](const Note& n)
        {
            return n.HasMidiNote() && n.part == part && Heard(n);
        };
        if (part == tr.part || !std::ranges::any_of(tr.notes, held))
        {
            continue;
        }

        LeftPart& left = tr.left[part];
        auto follow = [&](int& last, int value, EventKind kind)
        {
            if (value != last)
            {
                last = value;
                Emit(t, {0, kind, 0, static_cast<uint8_t>(value)}, part);
            }
        };
        follow(left.volume, std::clamp(tr.vol.Get(), 0, 127), EventKind::kVolume);
        follow(left.expression, std::clamp(static_cast<int>(std::lround(level * left.gain * level_scale_)), 0, 127),
               EventKind::kExpression);
        level_peak_ = std::max(level_peak_, level * left.gain);
        follow(left.pan, pan, EventKind::kPan);
        follow(left.mod, mod, EventKind::kModulation);
        follow(left.reverb, std::min<int>(127, tr.fx_send_a), EventKind::kReverb);
        follow(left.chorus, std::min<int>(127, tr.fx_send_b), EventKind::kChorus);
        if (filter_controller_[t])
        {
            follow(left.filter, FilterControllerValue(tr.filter.cutoff), EventKind::kFilter);
        }
        follow(left.sfz_low_pass, SfzLowPassValue(tr.low_pass), EventKind::kSfzLowPass);
        follow(left.sfz_biquad, biquad, EventKind::kSfzBiquad);
    }

    // MIDI has one pitch per channel; NW4R SND has one per note. For change detection, take the track's bend and the
    // newest sweep of any part, without the legato key offset. The game's notes have no parts (Track::last_change). A
    // note below hearing changes nothing that can be heard.
    float game_pitch = tr.Bend();
    for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
    {
        const float s = it->SweepAt(frame_);
        if (s != 0.0f && !BelowHearing(*it))
        {
            game_pitch += s;
            break;
        }
    }
    if (std::fabs(game_pitch - tr.change_pitch) > 0.0005f || (game_pitch != tr.change_pitch && game_pitch == 0.0f))
    {
        tr.change_pitch = game_pitch;
        tr.last_change = tick_;
    }

    // Each part gets the pitch of its notes, the track's first.
    if (SendPitch(t, tr.part, frame_) && tails)
    {
        Approximate("detached notes (still receive track controller changes in MIDI)");
    }
    for (uint8_t part = 0; part < 16; part++)
    {
        if (part != tr.part)
        {
            SendPitch(t, part, frame_);
        }
    }
}

std::optional<float> Performer::PartPitch(int t, uint8_t part, uint64_t frame) const
{
    const Track& tr = tracks_[t];
    if (part == tr.part && tr.open)
    {
        const float pitch = tr.Bend() + tr.legato_bend;
        for (auto it = tr.notes.rbegin(); it != tr.notes.rend(); ++it)
        {
            const float s = it->SweepAt(frame);
            if (s != 0.0f && it->part == part && it->HasMidiNote())
            {
                return pitch + s;
            }
        }

        return pitch;
    }

    const Note* newest = nullptr;
    bool held = false;
    float sweep = 0.0f;
    auto look = [&](const Note& n, bool holds)
    {
        if (!n.HasMidiNote() || n.part != part || !Heard(n))
        {
            return;
        }

        if (!newest)
        {
            newest = &n;
            held = holds;
        }
        sweep = sweep != 0.0f ? sweep : n.SweepAt(frame);
    };
    std::for_each(tr.notes.rbegin(), tr.notes.rend(), [&](const Note& n) { look(n, true); });
    std::for_each(tr.detached.rbegin(), tr.detached.rend(), [&](const Note& n) { look(n, false); });
    if (!newest)
    {
        return std::nullopt;
    }

    // The game gives the notes a track holds its bend in the frame of the tick that changes it (GiveTrackSettings comes
    // after this tick's events); detached notes keep theirs.
    const float bend = held ? tr.Bend() : newest->bend;
    return bend + static_cast<float>(newest->key - newest->midi_key) + sweep;
}

bool Performer::SendPitch(int t, uint8_t part, uint64_t frame, double after)
{
    Track& tr = tracks_[t];
    const std::optional<float> pitch = PartPitch(t, part, frame);
    float& sent = part == tr.part ? tr.out_pitch : tr.left[part].pitch;
    if (!pitch || !(std::fabs(*pitch - sent) > 0.0005f || (*pitch != sent && *pitch == 0.0f)))
    {
        return false;
    }

    // A pitch between ticks follows the tick's events, rather than taking the place of one of them (Emit).
    sent = *pitch;
    TrackEvent e{0, EventKind::kPitch};
    e.semitones = *pitch;
    if (after == 0)
    {
        Emit(t, e, part);
        return true;
    }

    e.tick = tick_;
    e.part = part;
    e.after = after;
    out_.tracks[t].push_back(e);

    return true;
}

bool Performer::TailsOnPart(int t) const
{
    const Track& tr = tracks_[t];
    auto shares_part = [&](const Note& n)
    {
        return n.HasMidiNote() && n.part == tr.part && StillAudible(n);
    };

    return std::ranges::any_of(tr.detached, shares_part);
}

void Performer::SweepBetweenTicks(const TickTime& next)
{
    // Channel::Update moves a timed sweep on every sound frame. That includes the frames between ticks and the frames
    // after the track has closed. The track's settings hold still between ticks. Find the parts with one under way.
    std::array<std::array<bool, 16>, 16> sweeping{};
    bool any = false;
    for (int t = 0; t < 16; t++)
    {
        const Track& tr = tracks_[t];
        if (!tr.played)
        {
            continue;
        }

        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.HasMidiNote() && n.sweep_in_ms && n.SweepAt(frame_) != 0.0f)
                {
                    sweeping[t][n.part] = true;
                    any = true;
                }
            }
        }
    }
    if (!any)
    {
        return;
    }

    // Send each frame's pitch where the frame starts. Give its time as a fraction of the way to the next tick. For the
    // build of October 2006, the fraction counts at MIDI's tempo from the tick's nominal time (RecordTickTime).
    for (uint64_t f = frame_ + 1; f < next.frame; f++)
    {
        const double start = static_cast<double>(f) * kFrameMs;
        const double after =
            build_.ax_2006 ? (start - nominal_ms_) / MidiMsPerTick() : (start - now_ms_) / (next.ms - now_ms_);
        for (int t = 0; t < 16; t++)
        {
            for (uint8_t part = 0; part < 16; part++)
            {
                const Track& tr = tracks_[t];
                if (sweeping[t][part] && SendPitch(t, part, f, after) && tr.open && part == tr.part && TailsOnPart(t))
                {
                    Approximate("detached notes (still receive track controller changes in MIDI)");
                }
            }
        }
    }
}

bool Performer::SweepMoves(int t, uint8_t part, uint64_t frame, double ms) const
{
    const Track& tr = tracks_[t];
    auto moves = [&](const Note& n)
    {
        return n.HasMidiNote() && n.part == part && n.sweep_in_ms && n.SweepAt(frame) != 0.0f && n.EndMs() > ms;
    };

    return std::ranges::any_of(tr.notes, moves) || std::ranges::any_of(tr.detached, moves);
}

void Performer::SweepAfterEnd()
{
    // As MsToTick counts, MIDI goes on at the last tick's tempo. Follow the sweeps for as long as the release tails
    // last (EndPerformance).
    const double per_tick = MsPerTick();
    if (per_tick <= 0)
    {
        return;
    }

    for (uint64_t f = frame_ + 1; static_cast<double>(f) * kFrameMs < now_ms_ + kMaxTailMs; f++)
    {
        const double ms = static_cast<double>(f) * kFrameMs;
        bool any = false;
        for (int t = 0; t < 16; t++)
        {
            for (uint8_t part = 0; part < 16; part++)
            {
                if (SweepMoves(t, part, f - 1, ms))
                {
                    SendPitch(t, part, f,
                              build_.ax_2006 ? (ms - nominal_ms_) / MidiMsPerTick() : (ms - now_ms_) / per_tick);
                    any = true;
                }
            }
        }
        if (!any)
        {
            break;
        }
    }
}

double Performer::TrackLevel(int t) const
{
    // The voice clamps a channel's volume to 1 (Voice::SetVolume). The track's volumes never take a note above full
    // level. Limit them to full level, the most CC7 and CC11 can give together.
    const Track& tr = tracks_[t];
    const double level = std::max(tr.vol.Get(), 127) * (tr.volume2 / 127.0) * (main_volume_ / 127.0) *
                         std::sqrt(std::min<int>(sound_.volume, 127) / 127.0);
    return std::min(level, 127.0 * 127.0 / std::clamp(tr.vol.Get(), 1, 127));
}

void Performer::StartSweep(int t, Note& n, int key, int32_t length)
{
    // SeqTrack::NoteOn: the sweep runs over the note's length, or with a portamento time over a time that grows with
    // the square of that time and with the interval, in milliseconds that Channel::Update counts down 3 at a time.
    Track& tr = tracks_[t];
    float sweep = tr.sweep_pitch;
    if (tr.porta)
    {
        sweep += static_cast<float>(static_cast<int>(tr.porta_key) - key);
    }

    n.sweep = sweep;
    n.sweep_ticks = 0;
    n.sweep_frame = frame_;
    if (tr.porta_time != 0)
    {
        int time = tr.porta_time * tr.porta_time;
        time = static_cast<int>(static_cast<float>(time) * (sweep >= 0.0f ? sweep : -sweep));
        time = static_cast<int>(static_cast<uint32_t>(time >> 5));
        n.sweep_length = (time << 2) + time;
        n.sweep_in_ms = true;
    }
    else
    {
        n.sweep_length = length > 0 ? length : 0;
        n.sweep_in_ms = false;
    }

    tr.porta_key = static_cast<uint8_t>(key);
}

void Performer::MidiNoteOn(int t, Note& n, int velocity, const Region* region, double gain)
{
    Track& tr = tracks_[t];
    n.midi_key = n.key;
    if (velocity <= 0)
    {
        return; // silent in NW4R SND; MIDI velocity 0 would be a note-off
    }

    // A note that requires a pitch or level that the other notes sounding on the track's part don't share moves the
    // track to another part (Switch). Each part has a MIDI channel of its own. A note that sweeps, or that starts
    // beside one still sweeping, moves the track. So does a note above velocity 127. A note that later legatos raise
    // moves the track too. Such a note starts higher (below). Last, a note moves the track if its CC11, or the CC11 a
    // legato lowered, would change the level of the others.
    const auto rise = start_velocities_.find(next_midi_id_);
    auto sweeps_otherwise = [&](const Note& other)
    {
        return &other != &n && other.HasMidiNote() && other.part == tr.part && Heard(other) &&
               !n.SweepsLike(other, frame_);
    };
    const bool own_settings = std::ranges::any_of(tr.notes, sweeps_otherwise) ||
                              std::ranges::any_of(tr.detached, sweeps_otherwise) || rise != start_velocities_.end() ||
                              velocity > 127 || gain != 1.0 || tr.legato_gain != 1.0;
    if (own_settings && !Alone(t, n))
    {
        Switch(t, &n);
    }
    n.part = tr.part;
    parts_used_[t] = std::max<uint8_t>(parts_used_[t], n.part + 1);

    // A note that legatos raise later (Rises) starts at the highest velocity that they reach and that its region still
    // plays. That way the synth selects the same zone, and CC11 brings the note down to the velocity it plays at. The
    // legatos then continue the note (Legato). That works only for a note that's alone on its part (Alone).
    const int capped = std::min(velocity, 127);
    int start = capped;
    n.liftable = Alone(t, n);
    if (n.liftable && rise != start_velocities_.end() && region)
    {
        for (int v = std::min(rise->second, 127); v > capped; v--)
        {
            if (banks_.FindRegion(n.program, n.key, v) == region)
            {
                start = v;
                break;
            }
        }
    }

    // A note above velocity 127 plays at 127. If the note is alone on its part, CC11 raises it the rest of the way.
    double above = 1.0;
    if (velocity > 127)
    {
        if (n.liftable)
        {
            above = static_cast<double>(velocity) / 127.0;
        }
        else
        {
            Approximate("velocity above 127 (played at 127)");
        }
    }

    tr.legato_gain = gain * capped / start * above;
    tr.legato_bend = 0.0f;
    UpdateOutputs(t); // the controllers, and the pitch of a sweeping note, come first

    if (tr.lfo_type == 2 && tr.lfo_depth != 0 && tr.lfo_delay >= 0)
    {
        Approximate("pan LFO (omitted from SoundFont)");
    }
    if (tr.lfo_type == 1 && tr.lfo_depth * tr.lfo_range > 128 && tr.lfo_delay >= 0)
    {
        Approximate("volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)");
    }
    const bool low_pass = LowPassIndex(tr.low_pass) >= 0;
    const int biquad_type = BiquadStep(tr.biquad_type, tr.biquad_value) >= 0 ? tr.biquad_type : 0;
    if (low_pass)
    {
        Approximate("low-pass filter (one-pole in the game, two-pole in the SoundFont)");
    }
    if (biquad_type >= 2)
    {
        Approximate("biquad high-pass or band-pass (omitted from SoundFont)");
    }
    if (low_pass && biquad_type == 1)
    {
        Approximate("combined low-pass filters (SoundFont uses the lower cutoff)");
    }
    if (biquad_type != 0 && biquad_type != out_.biquad_types[t])
    {
        Approximate("biquad type change (SFZ supports one type per track)");
    }
    if (tr.fx_send_c != 0)
    {
        Approximate("effect send C (omitted)");
    }

    const auto attack = attacks_.find(next_midi_id_);
    n.attack_timecents = attack != attacks_.end() ? std::optional<int16_t>(attack->second) : std::nullopt;
    n.preset = NoteKey(t, n);
    n.lfo_moved = false;
    const int preset = PresetIndex(n.preset);
    if (preset != tr.out_preset)
    {
        tr.out_preset = preset;
        TrackEvent ev{0, EventKind::kPreset};
        ev.preset = static_cast<uint32_t>(preset);
        Emit(t, ev);
    }

    // Assign IDs to overlapping notes of the same key so the MIDI writer can distribute them across channels.
    n.midi_on = true;
    n.midi_velocity = static_cast<uint8_t>(start);
    n.start_velocity = velocity;
    n.midi_id = next_midi_id_++;
    n.midi_frame = frame_;
    TrackEvent ev{0, EventKind::kNoteOn, n.midi_key, n.midi_velocity};
    ev.note = n.midi_id;
    Emit(t, ev);
    CheckFullLevel(t);
}

void Performer::NoteOn(int t, int key, int velocity, int32_t length)
{
    // SeqTrack::NoteOn (main.dol 0x80040bc0). From June 2008 the velocity is scaled by the track's velocity range over
    // 127 first, and can exceed 127 (Wii Music, 0x800bee00).
    Track& tr = tracks_[t];

    // At most kVoices new channels can sound in one tick, fewer with stereo waves. Cap new channels to avoid thousands
    // of MIDI notes from loops without waits. Legatos above the same cap continue their existing MIDI note.
    if (tr.note_tick != tick_)
    {
        tr.note_tick = tick_;
        tr.tick_notes = 0;
        tr.tick_legatos = 0;
    }

    const int vel = velocity * tr.velocity_range / 127;
    if (!tr.played)
    {
        tr.played = true;
        UpdateOutputs(t);
    }

    // Tie continues the newest channel, even one that is releasing; mono (from 2008) continues it unless it is
    // releasing, when it stops it and starts a new one (Mario Kart Wii, 0x8009be98). See Legato for MIDI handling.
    if (!tr.notes.empty() && (tr.tie || tr.mono))
    {
        Note& head = tr.notes.back();
        if (!tr.mono || !head.released)
        {
            Legato(t, head, key, vel, length, ++tr.tick_legatos > Voices());
            return;
        }

        StopNote(t, head);
        Ended(head);
        tr.notes.pop_back();
    }

    // Bank::NoteOn (main.dol 0x80035b68) finds the new channel's region and wave, or leaves it silent.
    const Region* region = banks_.FindRegion(tr.program, key, vel);
    if (!region)
    {
        return;
    }

    const Pcm* pcm = banks_.GetWave(static_cast<uint32_t>(region->wave_index));
    if (!pcm)
    {
        return;
    }

    if (++tr.tick_notes > Voices())
    {
        Approximate(build_.ax_2006 ? "more than 64 new channels on a track in one tick (excess omitted; the game keeps "
                                     "the newest)"
                                   : "more than 96 new channels on a track in one tick (excess omitted; the game keeps "
                                     "the newest)");
        return;
    }

    // NW4R SND's default pan curve applies.
    if (vel > 0 && sound_.pan_curve != 0)
    {
        Approximate("custom pan curve (using the default)");
    }

    // NW4R SND adds region pan, track initial pan and track pan before applying its pan law. The synth receives the
    // first two as zone pan and the third as CC10, then combines them by the synth's law.
    if (vel > 0 && region->pan + tr.init_pan != 64 && tr.pan.Get() != 0)
    {
        Approximate("combined region and track pan (using the synth pan law)");
    }

    // In balance mode a stereo wave's channels keep to their sides, but CC10 pans them as in dual mode.
    const bool stereo = pcm->channels.size() > 1;
    if (vel > 0 && stereo && balance_ && tr.pan.Get() != 0)
    {
        Approximate("track pan on stereo waves in balance mode (they pan as in dual mode)");
    }

    // The note's volume is its velocity squared times the region's volume, and its envelope the region's, with the
    // hold, the note-off type and the key group from 2008 (Mario Kart Wii's Bank::NoteOn, 0x8008d41c and 0x8008d490).
    Note n;
    n.key = static_cast<uint8_t>(key);
    n.length = tr.tie ? -1 : length;
    n.on_frame = frame_;
    n.ignore_note_off = build_.Since2008() && region->note_off_type == 1;
    n.key_group = build_.Since2008() ? region->key_group : 0;
    n.program = tr.program;
    n.init_pan = tr.init_pan;
    n.stereo = stereo;
    const float init_volume = static_cast<float>(vel) / 127.0f;
    n.init_volume = init_volume * init_volume * (static_cast<float>(region->volume) / 127.0f);
    n.env = OverrideEnvelope(RegionEnvelope(*region, build_.Since2008()), TrackPresetKey(tr));
    n.quiet_ms = n.QuietMs();
    n.faded_ms = n.FadedMs();

    n.original_key = region->original_key;
    n.tune = region->tune;
    n.sample_rate = pcm->sample_rate;
    n.midi_volume = region->volume;
    if (!pcm->loop)
    {
        n.StartWave(frame_, pcm->Length(), n.RatePerFrame(static_cast<float>(key - n.original_key)));
    }

    StartSweep(t, n, key, length);

    // From 2008, a note of a key group releases the track's other notes of the group (Mario Kart Wii, 0x8009bfcc).
    if (n.key_group != 0)
    {
        for (Note& other : tr.notes)
        {
            if (other.key_group == n.key_group)
            {
                ReleaseNote(t, other);
            }
        }
    }

    tr.notes.push_back(n);
    MidiNoteOn(t, tr.notes.back(), vel, region);
}

void Performer::Legato(int t, Note& n, int key, int velocity, int32_t length, bool past_cap)
{
    // The channel takes the new key and its velocity squared as its volume, and mono its length too; the track's
    // envelope settings and the sweep apply to it as to a new note.
    Track& tr = tracks_[t];
    const bool heard = !BelowHearing(n);
    n.key = static_cast<uint8_t>(key);
    if (tr.mono)
    {
        n.length = length;
    }
    const float init_volume = static_cast<float>(velocity) / 127.0f;
    n.init_volume = init_volume * init_volume;
    n.env = OverrideEnvelope(n.env, TrackPresetKey(tr));
    StartSweep(t, n, key, length);

    // A legato on a channel below hearing changes nothing that can be heard. Count every other legato as a change,
    // regardless of MIDI representation: that can differ between passes.
    if (heard)
    {
        n.quiet_ms = n.QuietMs();
        n.faded_ms = n.FadedMs();
        tr.last_change = tick_;
    }

    // A channel that has faded far enough goes on silent. Its MIDI note has ended (UpdateNoteLengths), and nothing
    // starts another.
    if (!heard && !n.midi_on)
    {
        return;
    }

    // SeqTrack::NoteOn sets gain from the new velocity without the region volume. The synth zone still applies region
    // volume. CC11 makes up for it. A zero-volume region cannot be made audible this way.
    auto without_region_volume = [&](double gain)
    {
        if (n.midi_volume == 0 && velocity > 0)
        {
            Approximate("legato on a zero-volume region (audible in the game, silent in the synth)");
        }
        return n.midi_volume > 0 ? gain * std::sqrt(127.0 / n.midi_volume) : gain;
    };

    // Continue the MIDI note if its preset matches. CC11 handles the velocity and pitch bend the key (UpdateOutputs).
    // The note keeps its sample position and original region. At velocity 0, keep the note silent through CC11
    // regardless of preset; ending it would leave a release tail.
    //
    // MIDI can't raise a note's velocity. A legato above the velocity that a MIDI note alone on its track started at
    // therefore raises CC11 above the note's start. Record it. A later pass then starts the note higher (MidiNoteOn);
    // CC11 above 127, past the top velocity of the note's region, lowers the whole sequence (Perform). A note that
    // isn't alone requires a new MIDI note. Lowering CC11 for it would lower the others too.
    //
    // After kVoices legatos in a tick, continue regardless of preset, capped at the starting velocity. Count only
    // legatos that would otherwise require a new MIDI note as approximations.
    //
    // A legato on a channel below hearing continues its MIDI note the same way. The game's legato doesn't start the
    // envelope again. A new MIDI note, or a later pass that starts the note higher, would change what can be heard. The
    // gain of such a legato therefore stays at or below the note's start, and the legato records no rise.
    //
    // After an envelope reset, the track leaves its envelope settings to the bank (0xff). In a legato, such settings
    // leave the channel's settings as they were (SeqTrack::NoteOn). They count as the MIDI note's settings.
    auto same_preset = [&]()
    {
        const PresetKey& sent = out_.presets[static_cast<std::size_t>(tr.out_preset)];
        PresetKey key = NoteKey(t, n);
        for (auto [kept, from] : {std::pair{&key.attack, &sent.attack}, std::pair{&key.decay, &sent.decay},
                                  std::pair{&key.sustain, &sent.sustain}, std::pair{&key.release, &sent.release},
                                  std::pair{&key.hold, &sent.hold}})
        {
            if (*kept == 0xff)
            {
                *kept = *from;
            }
        }

        return key == sent;
    };
    const bool carries_on =
        n.midi_on && (velocity <= n.midi_velocity || n.liftable) && (velocity <= 0 || same_preset());
    if (carries_on || (n.midi_on && (past_cap || !heard)))
    {
        if (!carries_on && heard)
        {
            Approximate(build_.ax_2006 ? "more than 64 legatos in a tick (keeping the MIDI preset and limiting gain to "
                                         "the starting level)"
                                       : "more than 96 legatos in a tick (keeping the MIDI preset and limiting gain to "
                                         "the starting level)");
        }
        else if (carries_on && heard && velocity > n.start_velocity)
        {
            int& top = rises_[n.midi_id];
            top = std::max(top, velocity);
        }

        const int kept = carries_on && heard ? std::max(velocity, 0) : std::clamp<int>(velocity, 0, n.midi_velocity);
        tr.legato_gain = without_region_volume(static_cast<double>(kept) / n.midi_velocity);
        tr.legato_bend = static_cast<float>(key - n.midi_key);
        return;
    }

    // Start a new MIDI note. Select its region by the new key and velocity. A legato at velocity 0 starts none.
    const Region* played = banks_.FindRegion(n.program, key, velocity);
    n.midi_volume = played ? played->volume : 127;
    EndMidiNote(t, n);
    MidiNoteOn(t, n, velocity, played, without_region_volume(1.0));
    if (n.midi_on)
    {
        Approximate("legato (tie or mono) played as a new note");
    }
}

void Performer::CommandProc(int t, uint32_t cmd, int32_t arg1, int32_t arg2)
{
    // MmlParser::CommandProc (main.dol 0x8003bee8). The builds of 2008 added commands. Older builds ignore them.
    if (cmd > 0xff) // variable and comparison commands (0xf0 prefix)
    {
        VariableCommand(t, cmd & 0xff, static_cast<uint8_t>(arg1), static_cast<int16_t>(arg2));
        return;
    }

    Track& tr = tracks_[t];
    const uint8_t a8 = static_cast<uint8_t>(arg1);
    switch (cmd)
    {
    case 0x81: // prg
        if (arg1 < 0x10000)
        {
            tr.program = static_cast<uint16_t>(arg1);
        }
        break;

    case 0x88: // opentrack: a track that reached FIN was freed and can't be opened again
        if (arg1 < 16 && arg1 != t && tracks_[arg1].allocated)
        {
            CloseTrack(arg1);
            OpenTrack(arg1, static_cast<uint32_t>(arg2));
        }
        break;

    case 0x89: // jump
        if (static_cast<uint32_t>(arg1) < tr.current)
        {
            OnLoop(t, static_cast<uint32_t>(arg1));
        }
        tr.current = static_cast<uint32_t>(arg1);
        break;

    case 0x8a: // call
        if (tr.call_depth >= 3)
        {
            break;
        }

        tr.call_stack[tr.call_depth] = tr.current;
        tr.call_depth++;
        tr.current = static_cast<uint32_t>(arg1);
        break;

    case 0xb0: // timebase: 0 stops sequence timing like tempo 0; MIDI keeps its timebase
        if (build_.ax_2006)
        {
            break; // the build of October 2006 has no timebase
        }

        timebase_ = a8;
        if (tick_ == 0 && timebase_ > 0)
        {
            out_.timebase = timebase_;
        }
        EmitTempo();
        break;

    case 0xb1: // env_hold, from 2008 (Mario Kart Wii, 0x80096b90)
        if (build_.Since2008())
        {
            tr.hold = a8;
        }
        break;

    case 0xb2: // monophonic, from 2008 (Mario Kart Wii, 0x80096be8)
        if (!build_.Since2008())
        {
            break;
        }

        tr.mono = arg1 != 0;
        if (tr.mono)
        {
            ReleaseAndDetach(t);
        }
        break;

    case 0xb3: // velocity range, from June 2008 (Wii Music, 0x800b95c4)
        if (build_.SinceJune2008())
        {
            tr.velocity_range = a8;
        }
        break;

    case 0xb4: // biquad type and value, from June 2008
        if (build_.SinceJune2008())
        {
            tr.biquad_type = a8;
            SetFilter(t);
        }
        break;

    case 0xb5:
        if (build_.SinceJune2008())
        {
            tr.biquad_value = a8;
            SetFilter(t);
        }
        break;

    case 0xc0:
        tr.pan.SetTarget(static_cast<int8_t>(PanArgument(arg1) - 0x40), arg2);
        break;

    case 0xc1:
        tr.vol.SetTarget(a8, arg2);
        break;

    case 0xc2:
        main_volume_ = a8;
        break;

    case 0xc3:
        tr.transpose = static_cast<int8_t>(arg1);
        break;

    case 0xc4: // pitch bend, at once in every build
        tr.bend = static_cast<int8_t>(arg1);
        break;

    case 0xc5:
        tr.bend_range = a8;
        break;

    case 0xc7:
        tr.note_wait = arg1 != 0;
        break;

    case 0xc8: // tie
        tr.tie = arg1 != 0;
        ReleaseAndDetach(t);
        break;

    case 0xc9: // porta (from a key)
        tr.porta = true;
        tr.porta_key = static_cast<uint8_t>(arg1 + tr.transpose);
        break;

    case 0xca:
        tr.lfo_depth = a8;
        break;

    case 0xcb:
        SetLfoSetting(t, tr.lfo_speed, a8);
        break;

    case 0xcc:
        SetLfoSetting(t, tr.lfo_type, a8);
        break;

    case 0xcd:
        SetLfoSetting(t, tr.lfo_range, a8);
        break;

    case 0xce:
        tr.porta = arg1 != 0;
        break;

    case 0xcf:
        tr.porta_time = a8;
        break;

    case 0xd0:
        tr.attack = a8;
        break;

    case 0xd1:
        tr.decay = a8;
        break;

    case 0xd2:
        tr.sustain = a8;
        break;

    case 0xd3:
        tr.release = a8;
        break;

    case 0xd4: // loop start
        if (tr.call_depth < 3)
        {
            tr.call_stack[tr.call_depth] = tr.current;
            tr.loop_count[tr.call_depth] = a8;
            tr.call_depth++;
        }
        break;

    case 0xd5:
        tr.volume2 = a8;
        break;

    case 0xd7: // surround pan: the game's stereo mix leaves it out
        tr.surround_pan.SetTarget(PanArgument(a8), arg2);
        break;

    case 0xd8:
        tr.low_pass = a8;
        SetFilter(t);
        break;

    case 0xd9:
        tr.fx_send_a = a8;
        break;

    case 0xda:
        tr.fx_send_b = a8;
        break;

    case 0xdb:
        if (a8 != 127)
        {
            Approximate("main send (omitted)");
        }
        break;

    case 0xdc:
        tr.init_pan = static_cast<int8_t>(PanArgument(arg1) - 0x40);
        break;

    case 0xdd:
        Mute(t, a8);
        break;

    case 0xde:
        tr.fx_send_c = a8;
        break;

    case 0xdf:
        tr.damper = a8 >= 0x40;
        break;

    case 0xe0:
        SetLfoSetting(t, tr.lfo_delay, arg1);
        break;

    case 0xe1: // tempo: from 2008 kept within 0-1023 (Mario Kart Wii, 0x80096924), before that 16 bits
        tempo_ = build_.Since2008() ? std::clamp(arg1, 0, 0x3ff) : static_cast<uint16_t>(arg1);
        EmitTempo();
        break;

    case 0xe3: // sweep pitch: in 32nds of a semitone, or 64ths from 2008 (Mario Kart Wii, 0x80096b58)
        tr.sweep_pitch = static_cast<float>(arg1) / (build_.Since2008() ? 64.0f : 32.0f);
        break;

    case 0xfb: // envelope reset, from June 2008 (Wii Music, 0x800b9788)
        if (build_.SinceJune2008())
        {
            tr.attack = tr.decay = tr.sustain = tr.release = 0xff;
            tr.hold = 0xff;
        }
        break;

    case 0xfc:
        LoopEnd(t);
        break;

    case 0xfd: // return
        if (tr.call_depth != 0)
        {
            tr.call_depth--;
            tr.current = tr.call_stack[tr.call_depth];
        }
        break;

    default:
        // 0xc6 priority, 0xd6 printvar: nothing to record.
        break;
    }
}

void Performer::VariableCommand(int t, uint32_t op, uint8_t index, int16_t v)
{
    int16_t* const var = Variable(t, index);
    if (!var)
    {
        return;
    }

    Track& tr = tracks_[t];
    const int16_t before = *var;
    if (op >= 0x90)
    {
        NoteVariableRead(t, index);
    }

    switch (op)
    {
    case 0x80:
        *var = v;
        break;

    case 0x81:
        *var = static_cast<int16_t>(*var + v);
        break;

    case 0x82:
        *var = static_cast<int16_t>(*var - v);
        break;

    case 0x83:
        *var = static_cast<int16_t>(*var * v);
        break;

    case 0x84:
        if (v != 0)
        {
            *var = static_cast<int16_t>(*var / v);
        }
        break;

    case 0x85: // PowerPC shifts use the low 6 bits of the amount
        *var = static_cast<int16_t>(v >= 0 ? ShiftLeft(*var, v) : ShiftRight(*var, -v));
        break;

    case 0x86:
        {
            int32_t range = v;
            const bool neg = range < 0;
            if (neg)
            {
                range = static_cast<int16_t>(-v);
            }

            const int32_t r =
                static_cast<int32_t>(static_cast<uint32_t>(rng_.Next()) * static_cast<uint32_t>(range + 1)) >> 16;
            last_draw_ = tick_;
            *var = static_cast<int16_t>(neg ? -r : r);
            break;
        }

    case 0x87:
        *var = static_cast<int16_t>(*var & v);
        break;

    case 0x88:
        *var = static_cast<int16_t>(*var | v);
        break;

    case 0x89:
        *var = static_cast<int16_t>(*var ^ v);
        break;

    case 0x8a:
        *var = static_cast<int16_t>(~v);
        break;

    case 0x8b:
        if (v != 0)
        {
            *var = static_cast<int16_t>(*var % v);
        }
        break;

    case 0x90:
        tr.cmp_flag = *var == v;
        break;

    case 0x91:
        tr.cmp_flag = *var >= v;
        break;

    case 0x92:
        tr.cmp_flag = *var > v;
        break;

    case 0x93:
        tr.cmp_flag = *var <= v;
        break;

    case 0x94:
        tr.cmp_flag = *var < v;
        break;

    case 0x95:
        tr.cmp_flag = *var != v;
        break;

    default:
        break;
    }

    if (*var != before)
    {
        NoteVariableChange(t, index);
    }
}

void Performer::NoteVariableRead(int t, uint8_t index)
{
    if (index < 32)
    {
        variable_readers_[index] |= static_cast<uint16_t>(1u << t);
    }
}

void Performer::NoteVariableChange(int t, uint8_t index)
{
    if (index >= 32)
    {
        return;
    }

    for (int i = 0; i < 16; i++)
    {
        if (i != t && (variable_readers_[index] >> i & 1) && tracks_[i].open && !WaitsForever(i))
        {
            tracks_[t].last_signal = commands_run_;
            return;
        }
    }
}

void Performer::Mute(int t, uint8_t mode)
{
    // SeqTrack::SetMute (main.dol 0x80040a28).
    Track& tr = tracks_[t];
    switch (mode)
    {
    case 0:
        tr.mute = false;
        break;

    case 1:
        tr.mute = true;
        break;

    case 2: // release the channels
        ReleaseAndDetach(t);
        tr.mute = true;
        break;

    case 3: // stop the channels
        {
            // CC120 stops the parts that the track's notes sounded on. Tails that the track detached on other parts go
            // on.
            std::set<uint8_t> sounding;
            for (Note& n : tr.notes)
            {
                if (n.HasMidiNote() && n.EndMs() > now_ms_ && !Unheard(n))
                {
                    sounding.insert(n.part);
                }
                EndMidiNote(t, n);
                n.released = true;
                n.release_end_ms = now_ms_;
                Ended(n);
            }
            for (const uint8_t part : sounding)
            {
                Emit(t, {0, EventKind::kSoundOff}, part);
            }

            tr.notes.clear();
            tr.mute = true;
            break;
        }

    default:
        break;
    }
}

void Performer::LoopEnd(int t)
{
    // NW4R SND takes the top of the call stack as the loop, whether a loop start or a call put it there.
    Track& tr = tracks_[t];
    if (tr.call_depth == 0)
    {
        return;
    }

    // Only a count of 0 loops indefinitely and can become the main loop.
    uint8_t& count = tr.loop_count[tr.call_depth - 1];
    if (count != 0 && --count == 0)
    {
        tr.call_depth--;
        return;
    }
    if (count == 0)
    {
        OnLoop(t, tr.call_stack[tr.call_depth - 1]);
    }

    tr.current = tr.call_stack[tr.call_depth - 1];
}

void Performer::OnLoop(int t, uint32_t target)
{
    // A backward jump to unvisited commands is not a loop; shared code may precede its callers. Also ignore passes
    // without waits, such as random retries or variable polling (ParseNextTick).
    const auto run = last_run_[t].find(target);
    const std::optional<uint32_t> last_wait = tracks_[t].last_wait;
    if (run == last_run_[t].end() || !last_wait || *last_wait < run->second.tick)
    {
        return;
    }

    // A pass that changed nothing is idle: the track only waited, polled or counted. Changing a variable that another
    // open track reads counts as a change (Track::last_signal). It belongs to the pass if it came after the pass's
    // first command. The tick doesn't show that: a loop's last commands run in the tick that its next pass starts in.
    const uint32_t start = run->second.tick;
    const uint64_t first_command = run->second.order;
    auto changed = [&](int i)
    {
        const Track& other = tracks_[i];
        return (other.last_change && *other.last_change >= start) ||
               (other.last_signal && *other.last_signal > first_command);
    };
    Track& tr = tracks_[t];
    tr.idle = !changed(t);

    // Use the lowest-numbered track that can still advance: track 0 while active, or a later track for continuing sound
    // effects. A pass counts only if no earlier track contributed during it. The marked section then contains only
    // repeating material. A track whose last pass was idle doesn't count as advancing. An idle pass of this track
    // counts only if no later track advances or contributed either. A later track that has been quiet for kHoldSeconds
    // doesn't count as advancing: it may wait minutes in silence before it loops or closes.
    auto plays_on = [&](int i)
    {
        return tracks_[i].open && !WaitsForever(i) && !tracks_[i].idle && (i < t || !Quiet(i));
    };
    for (int i = 0; i < 16; i++)
    {
        if (i != t && (i < t || tr.idle) && (plays_on(i) || changed(i)))
        {
            return;
        }
    }

    // A pass that changed something, of another loop than the marked one, means the marked pass doesn't repeat what
    // plays now. That happens when an earlier track has gone idle while a later track's loop plays on. Mark this pass
    // instead. An idle pass counts only while nothing else changes. Any idle pass then repeats the sound. Such a pass
    // therefore keeps the marked loop.
    const std::pair<int, uint32_t> owner{t, target};
    if (!loop_owner_ || (!tr.idle && *loop_owner_ != owner))
    {
        out_.loop.reset();
        out_.loops = 0;
        loop_owner_ = owner;
        owner_passes_.clear();
        marked_ = 0;
        extended_ = false;
    }

    if (owner == *loop_owner_)
    {
        owner_passes_.emplace_back(start, tick_);
    }

    out_.loops++;
    if (!out_.loop)
    {
        out_.loop = Performance::Loop{start, tick_};
    }

    if (out_.loops >= options_.loops && now_ms_ >= kMinLoopSeconds * 1000.0 && MarkSteadyPass(t, target))
    {
        stop_ = true;
        stopped_by_ = t;
        held_sound_end_ms_ = HeldSoundEnd(start);
    }
}

std::optional<double> Performer::HeldSoundEnd(uint32_t start) const
{
    if (now_ms_ - tick_ms_[start] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Find the last change: the last MIDI event or tempo change.
    uint32_t last = 0;
    for (const std::vector<TrackEvent>& events : out_.tracks)
    {
        if (!events.empty())
        {
            last = std::max(last, events.back().tick);
        }
    }
    if (!out_.tempo.empty())
    {
        last = std::max(last, out_.tempo.back().tick);
    }
    if (now_ms_ - tick_ms_[last] <= kHoldSeconds * 1000.0)
    {
        return std::nullopt;
    }

    // Every sounding note must be endless, unreleased and the newest note of an open track with tie or mono on.
    bool sounding = false;
    for (const Track& tr : tracks_)
    {
        for (const Note& n : tr.detached)
        {
            if (n.EndMs() > now_ms_)
            {
                return std::nullopt;
            }
        }
        for (std::size_t k = 0; k < tr.notes.size(); k++)
        {
            const Note& n = tr.notes[k];
            if (n.EndMs() <= now_ms_ || BelowHearing(n))
            {
                continue;
            }
            if (!tr.open || !(tr.tie || tr.mono) || n.released || n.EndMs() < kNever || k + 1 != tr.notes.size())
            {
                return std::nullopt;
            }

            sounding = true;
        }
    }
    if (!sounding)
    {
        return std::nullopt;
    }

    return tick_ms_[last] + kHoldSeconds * 1000.0;
}

bool Performer::SilentLoop() const
{
    const uint32_t start = out_.loop->start;
    for (const std::vector<TrackEvent>& events : out_.tracks)
    {
        const auto starts_note = [start](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn && e.tick >= start;
        };
        if (std::ranges::any_of(events, starts_note))
        {
            return false;
        }
    }

    // A sunk channel (Note::sunk) has ended its MIDI note, and its sound ends at its fade.
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                if (n.midi_on || n.SoundEndMs() >= kNever)
                {
                    return false;
                }
            }
        }
    }

    return true;
}

bool Performer::Parse(int t)
{
    // MmlParser::Parse (main.dol 0x8003b8b8).
    Track& tr = tracks_[t];
    last_run_[t][tr.current] = {tick_, commands_run_++};
    tr.held_wait = false;

    // Prefixes specify the condition (if), from 2008 the encoding of a time that the commands of 0xb0-0xdf read after
    // their argument (Mario Kart Wii, 0x80096240), and the main argument's encoding.
    ArgType arg_type = kArgNone;
    ArgType time_type = kArgNone;
    bool cond = true;
    uint32_t cmd = ReadByte(t);
    if (cmd == 0xa2) // if
    {
        cmd = ReadByte(t);
        cond = tr.cmp_flag;
    }

    if (build_.Since2008() && cmd == 0xa3)
    {
        time_type = kArgS16;
        cmd = ReadByte(t);
    }
    else if (build_.Since2008() && cmd == 0xa4)
    {
        time_type = kArgRandom;
        cmd = ReadByte(t);
    }
    else if (build_.Since2008() && cmd == 0xa5)
    {
        time_type = kArgVariable;
        cmd = ReadByte(t);
    }

    if (cmd == 0xa0)
    {
        cmd = ReadByte(t);
        arg_type = kArgRandom;
    }
    if (cmd == 0xa1)
    {
        cmd = ReadByte(t);
        arg_type = kArgVariable;
    }

    // Read the main argument using the prefix encoding, or `encoding` by default.
    auto arg = [&](ArgType encoding)
    {
        return ReadArg(t, arg_type != kArgNone ? arg_type : encoding);
    };

    if ((cmd & 0x80) == 0) // note: key, velocity, length
    {
        const uint8_t velocity = ReadByte(t);
        const int32_t length = arg(kArgVmidi);
        if (!cond)
        {
            return false;
        }

        const int key = std::clamp(static_cast<int>(tr.transpose) + static_cast<int>(cmd), 0, 127);
        if (!tr.mute)
        {
            NoteOn(t, key, velocity, length > 0 ? length : -1);
        }

        if (tr.note_wait)
        {
            tr.wait = length;
            tr.held_wait = length >= kHeldTicks;
            if (length == 0)
            {
                tr.note_finish_wait = true;
            }
        }

        return false;
    }

    switch ((cmd & 0xf0) >> 4)
    {
    case 0x8:
        switch (cmd)
        {
        case 0x80: // wait
            {
                const int32_t w = arg(kArgVmidi);
                if (cond)
                {
                    tr.wait = w;
                }
                return false;
            }

        case 0x81: // prg
            {
                const int32_t prg = arg(kArgVmidi);
                if (cond)
                {
                    CommandProc(t, cmd, prg, 0);
                }
                return false;
            }

        case 0x88: // opentrack
            {
                const uint32_t track_no = ReadByte(t);
                uint32_t off = ReadByte(t);
                off = (off << 8) | ReadByte(t);
                off = (off << 8) | ReadByte(t);
                if (cond)
                {
                    CommandProc(t, cmd, static_cast<int32_t>(track_no), static_cast<int32_t>(off));
                }
                return false;
            }

        case 0x89:
        case 0x8a: // jump, call
            {
                uint32_t off = ReadByte(t);
                off = (off << 8) | ReadByte(t);
                off = (off << 8) | ReadByte(t);
                if (cond)
                {
                    CommandProc(t, cmd, static_cast<int32_t>(off), 0);
                }
                return false;
            }

        default:
            return false;
        }

    case 0x9:
        if (cond)
        {
            CommandProc(t, cmd, 0, 0);
        }
        return false;

    case 0xa:
        return false;

    case 0xb:
    case 0xc:
    case 0xd:
        {
            const uint32_t v = static_cast<uint32_t>(arg(kArgU8)) & 0xff;
            int32_t time = 0;
            if (time_type != kArgNone)
            {
                time = ReadArg(t, time_type);
            }
            if (!cond)
            {
                return false;
            }

            const int32_t a1 = (cmd == 0xc3 || cmd == 0xc4) ? static_cast<int8_t>(v) : static_cast<int32_t>(v);
            CommandProc(t, cmd, a1, time);
            return false;
        }

    case 0xe:
        {
            const int32_t v = static_cast<int16_t>(arg(kArgS16));
            if (cond)
            {
                CommandProc(t, cmd, v, 0);
            }
            return false;
        }

    case 0xf:
        if (cmd == 0xf0)
        {
            const uint32_t sub = ReadByte(t);
            const uint32_t kind = sub & 0xf0;
            if (kind == 0x80 || kind == 0x90)
            {
                const uint32_t var_no = ReadByte(t);
                const int32_t v = static_cast<int16_t>(arg(kArgS16));
                if (cond)
                {
                    CommandProc(t, 0xf000 + sub, static_cast<int32_t>(var_no), v);
                }
                return false;
            }

            if (kind == 0xe0)
            {
                arg(kArgS16); // userproc calls into the game; consume its argument
            }
            return false;
        }

        if (cmd == 0xfe) // alloctrack: the sound's flags allocate the tracks
        {
            ReadByte(t);
            ReadByte(t);
            return false;
        }

        if (cmd == 0xff) // fin, whatever the condition says
        {
            return true;
        }

        if (cond)
        {
            CommandProc(t, cmd, 0, 0);
        }
        return false;

    default:
        return false;
    }
}

bool Performer::ParseNextTick(int t)
{
    // SeqTrack::ParseNextTick (main.dol 0x80040190). From 2008 the moving settings move on a tick first (Mario Kart
    // Wii, 0x8009b280).
    Track& tr = tracks_[t];
    tr.vol.Update();
    tr.pan.Update();
    tr.surround_pan.Update();

    if (tr.note_finish_wait)
    {
        if (TrackBusy(t))
        {
            return false;
        }

        tr.note_finish_wait = false;
    }

    if (tr.wait > 0)
    {
        tr.wait--;
        if (tr.wait > 0)
        {
            return false;
        }
    }

    return ParseUntilWait(t);
}

bool Performer::ParseUntilWait(int t)
{
    // NW4R SND parses until a command waits, and hangs on a track that never does; stop such a track after kParseLimit
    // commands.
    Track& tr = tracks_[t];
    int count = 0;
    while (tr.wait == 0 && !tr.note_finish_wait && !stop_)
    {
        if (++count > kParseLimit)
        {
            out_.warnings.push_back("track " + std::to_string(t) + " stopped after running without waits");
            return true;
        }
        if (Parse(t))
        {
            return true;
        }
    }

    tr.last_wait = tick_;

    return false;
}

void Performer::AllocateTracks()
{
    for (int t = 0; t < 16; t++)
    {
        tracks_[t].Init();
        tracks_[t].allocated = (sound_.allocate_track_flags >> t) & 1;
    }
}

void Performer::StartTick()
{
    // SoundThread's frame (main.dol 0x80047e68) advances the random generator after the players and channels, once per
    // frame. Random arguments therefore depend on timing. Process each tick in the frame containing it.
    const uint64_t frame = frame_;
    for (; rng_frame_ < frame; rng_frame_++)
    {
        rng_.Next();
    }

    // Update all frames since the previous tick, including its frame.
    UpdateOneShots(frame);
}

bool Performer::ParseTracks(int first)
{
    bool any_open = false;
    for (int t = first; t < 16 && !stop_; t++)
    {
        Track& tr = tracks_[t];
        if (!tr.open)
        {
            continue;
        }

        UpdateNoteLengths(t);
        if (ParseNextTick(t))
        {
            CloseTrack(t);
            tr.allocated = false; // the player frees a finished track
            continue;
        }

        any_open = true;
    }

    return any_open;
}

void Performer::FinishTick()
{
    for (int t = 0; t < 16; t++)
    {
        UpdateOutputs(t);
        CheckLfo(t);
        if (tracks_[t].played && tracks_[t].open && TrackBusy(t))
        {
            level_peak_ = std::max(level_peak_, TrackLevel(t) * tracks_[t].legato_gain);
        }
        if (!tracks_[t].open)
        {
            PruneNotes(t);
        }
    }

    // The build of October 2006 stops a released note once its volume times its envelope is 0, whether a track owns it
    // or not (Channel::Update, 0x800e039c in Smooth Moves' main.dol). The later builds stop a note that a track owns
    // only when its envelope is silent. The channels update after the frame's last tick.
    if (build_.ax_2006 && frame_ticks_left_ == 0)
    {
        for (int t = 0; t < 16; t++)
        {
            const float user = UserVolume(t);
            for (Note& n : tracks_[t].notes)
            {
                if (n.released && n.EndMs() > now_ms_ && n.init_volume * user == 0.0f)
                {
                    StopNote(t, n);
                }
            }
        }
    }
}

void Performer::AdvanceTo(const TickTime& next)
{
    SweepBetweenTicks(next);
    frame_ = next.frame;
    rest_ = next.rest;
    now_ms_ = next.ms;
    if (build_.ax_2006)
    {
        tick_units_ = next.units;
        frame_ticks_left_ = next.ticks_left;
        nominal_ms_ = next.nominal_ms;
    }
    tick_++;
    RecordTickTime();
}

Performance Performer::Run()
{
    // SeqPlayer::Start (main.dol 0x8003e504) opens track 0, if the sound allocates it, at the sound's start offset.
    AllocateTracks();
    if (tracks_[0].allocated)
    {
        OpenTrack(0, sound_.start_offset);
    }

    // The build of October 2006's count starts at a whole tick. So the first frame runs one tick.
    if (build_.ax_2006)
    {
        frame_ticks_left_ = StartFrameOctober2006(tick_units_, tempo_) - 1;
    }
    EmitTempo();
    RecordTickTime();

    bool finished = false;
    double steady_since = -1.0;
    PassStart next_pass;
    for (;;)
    {
        StartTick();
        const bool any_open = ParseTracks(0);
        if (stop_)
        {
            // A main loop that sounds nothing is no loop. The sequence ends with its last sound, like one that ends.
            if (out_.loop && !held_sound_end_ms_ && SilentLoop())
            {
                finished = true;
                silent_loop_ = true;
                out_.loop.reset();
                out_.loops = 0;
            }
            else if (out_.loop && !held_sound_end_ms_)
            {
                next_pass = tick_ > out_.loop->end ? PassStartAt(out_.loop->end)
                                                   : NextPassStart(stopped_by_, tracks_[stopped_by_].current);
            }
            break;
        }

        FinishTick();

        // A sequence that ends has no main loop. A loop marked on the way doesn't count.
        if (!any_open || Stuck())
        {
            finished = true;
            out_.loop.reset();
            out_.loops = 0;
            break;
        }

        if (steady_since < 0 && Steady())
        {
            steady_since = now_ms_;
        }
        if (steady_since >= 0 && now_ms_ - steady_since >= kHoldSeconds * 1000.0)
        {
            out_.holds = true;
            break;
        }

        // Treat a sequence that has been silent for longer than kHoldSeconds at the time limit as finished. Such a
        // sequence ends with its last sound.
        if (now_ms_ >= kMaxSeconds * 1000.0)
        {
            bool sounding = false;
            for (int t = 0; t < 16; t++)
            {
                sounding = sounding || OthersSounding(t, nullptr);
            }
            finished = !sounding && now_ms_ - sound_end_ms_ > kHoldSeconds * 1000.0;
            out_.truncated = !finished;
            break;
        }

        const std::optional<TickTime> next = NextTick();
        if (!next)
        {
            out_.warnings.push_back(timebase_ == 0 ? "timebase 0: the sequence stops advancing"
                                                   : "tempo 0: the sequence stops advancing");
            PlayOutStopped();
            break;
        }

        AdvanceTo(*next);
    }

    EndPerformance(finished);
    if (out_.loop)
    {
        RestoreAtLoopStart(next_pass);
    }

    return out_;
}

std::map<Performer::SettingKey, TrackEvent> Performer::SettingsAt(const Performance& perf, uint32_t tick)
{
    // A track's first part starts at its key's pitch without an event (Track::out_pitch).
    std::map<SettingKey, TrackEvent> settings;
    for (int t = 0; t < 16; t++)
    {
        settings[{t, 0, EventKind::kPitch}] = TrackEvent{0, EventKind::kPitch};
        for (const TrackEvent& e : perf.tracks[t])
        {
            if (e.tick > tick || (e.tick == tick && e.after > 0))
            {
                break;
            }

            if (IsTrackSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                settings[{t, e.part, e.kind}] = e;
            }
        }
    }

    return settings;
}

Performer::PassStart Performer::PassStartAt(uint32_t tick) const
{
    PassStart start;
    start.settings = SettingsAt(out_, tick);

    for (const Performance::Tempo& tempo : out_.tempo)
    {
        if (tempo.tick > tick)
        {
            break;
        }

        start.bpm = tempo.bpm;
    }

    for (const auto& [switch_tick, t, part] : part_switches_)
    {
        if (switch_tick > tick)
        {
            break;
        }

        start.parts[static_cast<std::size_t>(t)] = part;
    }

    return start;
}

Performer Performer::PlayOn(int t, uint32_t target, uint32_t until) const
{
    // Track `t` has jumped back, and the tracks after it haven't run this tick. The copy runs them, then the ticks
    // after, with no limit on the loops. No pass then stops the copy again.
    Performer probe = *this;
    probe.stop_ = false;
    probe.options_.loops = std::numeric_limits<int>::max();

    probe.tracks_[t].current = target;
    if (probe.ParseUntilWait(t))
    {
        probe.CloseTrack(t);
        probe.tracks_[t].allocated = false;
    }
    probe.ParseTracks(t + 1);
    probe.FinishTick();

    while (probe.tick_ + 1 < until)
    {
        const std::optional<TickTime> next = probe.NextTick();
        if (!next)
        {
            break;
        }

        probe.AdvanceTo(*next);
        probe.StartTick();
        const bool any_open = probe.ParseTracks(0);
        probe.FinishTick();
        if (!any_open)
        {
            break;
        }
    }

    return probe;
}

Performer::PassStart Performer::NextPassStart(int t, uint32_t target) const
{
    return PlayOn(t, target, tick_ + 1).PassStartAt(tick_);
}

bool Performer::SameSetting(const TrackEvent& a, const TrackEvent& b)
{
    switch (a.kind)
    {
    case EventKind::kPitch:
        return a.semitones == b.semitones;
    case EventKind::kPreset:
        return a.preset == b.preset;
    default:
        return a.value == b.value;
    }
}

bool Performer::RepeatsAsTheGame(uint32_t start, uint32_t end) const
{
    const PassStart at_start = PassStartAt(start);
    const PassStart next = PassStartAt(end);
    const uint32_t length = end - start;

    // The tempo: a repeat starts with the tempo at the loop's end. A tempo event on the loop's first tick replaces that
    // with the start's tempo. When the game's next pass starts with the start's tempo, RestoreAtLoopStart sends that
    // tempo again, and the repeat starts with it too.
    double end_bpm = at_start.bpm;
    bool tempo_sent = false;
    for (const Performance::Tempo& tempo : out_.tempo)
    {
        tempo_sent = tempo_sent || tempo.tick == start;
        if (tempo.tick < end)
        {
            end_bpm = tempo.bpm;
        }
    }
    const double repeat_bpm = tempo_sent || next.bpm == at_start.bpm ? at_start.bpm : end_bpm;

    auto bpm_at = [&](uint32_t tick, uint32_t since, double otherwise)
    {
        double bpm = otherwise;
        for (const Performance::Tempo& tempo : out_.tempo)
        {
            if (tempo.tick > tick)
            {
                break;
            }

            if (tempo.tick >= since)
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    // A note of a pass: its tick in the pass, key, velocity and part, and the settings of its part that it plays with.
    // For a note of the loop, those are the settings that the pass sends before the note. The others come from where
    // the player loops from.
    struct Strike
    {
        uint32_t offset = 0;
        uint8_t key = 0;
        uint8_t velocity = 0;
        uint8_t part = 0;
        std::map<EventKind, TrackEvent> settings;
    };

    for (int t = 0; t < 16; t++)
    {
        const auto track = static_cast<std::size_t>(t);
        std::vector<Strike> loop;
        std::vector<Strike> game;
        std::array<std::map<EventKind, TrackEvent>, 16> settings{}; // each part's settings so far in the events
        std::array<std::map<EventKind, TrackEvent>, 16> sent{};     // each part's since the loop's start
        std::array<std::map<EventKind, TrackEvent>, 16> at_end{};   // each part's at the loop's end
        std::array<std::set<EventKind>, 16> in_first_tick{};        // each part's sent in the loop's first tick

        // A track's first part starts at its key's pitch and with the SFZ filters off, without events
        // (Track::out_pitch, Track::out_sfz_low_pass and Track::out_sfz_biquad).
        settings[0][EventKind::kPitch] = TrackEvent{0, EventKind::kPitch};
        settings[0][EventKind::kSfzLowPass] = TrackEvent{0, EventKind::kSfzLowPass};
        settings[0][EventKind::kSfzBiquad] = TrackEvent{0, EventKind::kSfzBiquad};

        bool ended = false;
        for (const TrackEvent& e : out_.tracks[track])
        {
            if (e.tick >= end + length)
            {
                break;
            }

            if (!ended && e.tick >= end)
            {
                at_end = settings;
                ended = true;
            }
            if (IsTrackSetting(e.kind) || e.kind == EventKind::kPreset)
            {
                settings[e.part][e.kind] = e;
                if (e.tick >= start)
                {
                    sent[e.part][e.kind] = e;
                }
                if (e.tick == start && e.after == 0)
                {
                    in_first_tick[e.part].insert(e.kind);
                }
            }
            else if (e.kind == EventKind::kNoteOn && e.tick >= start)
            {
                const bool in_loop = e.tick < end;
                (in_loop ? loop : game)
                    .push_back({e.tick - (in_loop ? start : end), e.key, e.value, e.part,
                                in_loop ? sent[e.part] : settings[e.part]});
            }
        }
        if (!ended)
        {
            at_end = settings;
        }

        if (loop.size() != game.size())
        {
            return false;
        }

        // What a repeat starts with on a part where the pass hasn't sent a setting: the loop end's value, or the loop
        // start's value where RestoreAtLoopStart sends it again. On the part that takes the track's new notes,
        // RestoreAtLoopStart sends it if the loop's first tick doesn't.
        auto repeat_value = [&](uint8_t part, EventKind kind) -> const TrackEvent*
        {
            const auto first = at_start.settings.find({t, part, kind});
            const auto again = next.settings.find({t, next.parts[track], kind});
            if (part == at_start.parts[track] && !in_first_tick[part].contains(kind) &&
                first != at_start.settings.end() && again != next.settings.end() &&
                SameSetting(first->second, again->second))
            {
                return &first->second;
            }

            const auto kept = at_end[part].find(kind);
            return kept != at_end[part].end() ? &kept->second : nullptr;
        };

        for (std::size_t k = 0; k < loop.size(); k++)
        {
            const Strike& a = loop[k];
            const Strike& b = game[k];
            if (a.offset != b.offset || a.key != b.key || a.velocity != b.velocity ||
                bpm_at(start + a.offset, start, repeat_bpm) != bpm_at(end + b.offset, 0, at_start.bpm))
            {
                return false;
            }

            std::set<EventKind> kinds;
            const std::map<EventKind, TrackEvent>& kept = at_end[a.part];
            for (const auto* list : {&a.settings, &b.settings, &kept})
            {
                for (const auto& [kind, e] : *list)
                {
                    kinds.insert(kind);
                }
            }

            for (EventKind kind : kinds)
            {
                const auto sent_before = a.settings.find(kind);
                const TrackEvent* repeat =
                    sent_before != a.settings.end() ? &sent_before->second : repeat_value(a.part, kind);
                const auto played = b.settings.find(kind);
                if ((repeat == nullptr) != (played == b.settings.end()) ||
                    (repeat && !SameSetting(*repeat, played->second)))
                {
                    return false;
                }
            }
        }
    }

    return true;
}

Performer::LoopChoice Performer::ChooseSteadyPass(int t, uint32_t target) const
{
    // Settings that the first pass's setup leaves, or that the game keeps from pass to pass, can make a pass play
    // otherwise than the pass after it. Notes that only the first pass strikes and later passes hold can do the same. A
    // loop that draws random numbers plays each pass otherwise anyway. Such a loop keeps its first pass.
    if (owner_passes_.empty() || (last_draw_ && *last_draw_ >= owner_passes_[0].first))
    {
        return {true, std::nullopt, false};
    }

    // A copy of this performer plays the two passes after the owner's last pass. Each pass and the next then have the
    // pass after them to compare with.
    const auto [last_start, last_end] = owner_passes_.back();
    const uint32_t length = last_end - last_start;
    const Performer probe = PlayOn(t, target, last_end + 2 * length);
    for (std::size_t i = marked_; i < owner_passes_.size(); i++)
    {
        const auto [start, end] = owner_passes_[i];
        if (probe.RepeatsAsTheGame(start, end))
        {
            return {static_cast<int>(owner_passes_.size() - i) >= options_.loops, i, false};
        }
    }

    // No pass so far plays as the next one does. If the next pass plays as the one after it, play it too, and otherwise
    // keep the first.
    if (!extended_ && probe.RepeatsAsTheGame(last_end, last_end + length))
    {
        return {false, std::nullopt, true};
    }

    return {true, std::nullopt, false};
}

bool Performer::MarkSteadyPass(int t, uint32_t target)
{
    // A later run of the sequence makes the choices of the first (Perform). The first run compared its passes without
    // the details that later runs give the notes it played, such as the attacks of early releases (EarlyReleases).
    // Notes that only a copy played lack those details.
    const LoopChoice choice = replayed_ < replay_.size() ? replay_[replayed_++] : ChooseSteadyPass(t, target);
    choices_.push_back(choice);
    if (choice.marked && *choice.marked < owner_passes_.size())
    {
        marked_ = *choice.marked;
        const auto [start, end] = owner_passes_[marked_];
        out_.loop = Performance::Loop{start, end};
        out_.loops = static_cast<int>(owner_passes_.size() - marked_);
    }

    extended_ = extended_ || choice.extend;

    return choice.stop;
}

void Performer::RestoreAtLoopStart(const PassStart& next_pass)
{
    const uint32_t start = out_.loop->start;
    const uint32_t end = out_.loop->end;
    const PassStart at_loop_start = PassStartAt(start);

    // The tempo is handled like the settings below. The tempo's changes are in one list, sorted by tick.
    auto tempo_at = [&](uint32_t tick, bool in_tick)
    {
        double bpm = at_loop_start.bpm;
        for (const Performance::Tempo& tempo : out_.tempo)
        {
            if (tempo.tick > start && (tempo.tick < tick || (in_tick && tempo.tick == tick)))
            {
                bpm = tempo.bpm;
            }
        }

        return bpm;
    };

    const bool tempo_sent = std::ranges::any_of(out_.tempo, [start](const auto& tempo) { return tempo.tick == start; });
    if (!tempo_sent && next_pass.bpm == at_loop_start.bpm &&
        (tempo_at(end, false) != at_loop_start.bpm || tempo_at(end, true) != at_loop_start.bpm))
    {
        auto later = [start](const Performance::Tempo& tempo)
        {
            return tempo.tick > start;
        };
        out_.tempo.insert(std::ranges::find_if(out_.tempo, later), {start, at_loop_start.bpm});
    }

    for (const auto& [key, at_start] : at_loop_start.settings)
    {
        // The game gives the setting back its value at the loop's start. Only the part that the track's new notes go to
        // counts. A track sends every setting again when it moves to another part (Switch).
        const auto [t, part, kind] = key;
        const auto track = static_cast<std::size_t>(t);
        if (part != at_loop_start.parts[track])
        {
            continue;
        }
        const auto next = next_pass.settings.find({t, next_pass.parts[track], kind});
        if (next == next_pass.settings.end() || !SameSetting(next->second, at_start))
        {
            continue;
        }

        // It isn't sent at the loop's start, and the loop ends with another value. A player may take the loop end's
        // tick or leave it out. Both count.
        std::vector<TrackEvent>& events = out_.tracks[track];
        bool sent = false;
        const TrackEvent* before_end = &at_start;
        const TrackEvent* at_end = &at_start;
        for (const TrackEvent& e : events)
        {
            if (e.part != part || e.kind != kind || e.tick < start || e.tick > end || (e.tick == end && e.after > 0))
            {
                continue;
            }

            if (e.tick == start && e.after == 0)
            {
                sent = true;
            }
            if (e.tick < end)
            {
                before_end = &e;
            }
            at_end = &e;
        }
        if (sent || (SameSetting(*before_end, at_start) && SameSetting(*at_end, at_start)))
        {
            continue;
        }

        // Send it with the loop start's other settings, ahead of its notes.
        auto past_settings = [start](const TrackEvent& e)
        {
            return e.tick > start || (e.tick == start && !IsTrackSetting(e.kind));
        };
        const auto at = std::find_if(events.begin(), events.end(), past_settings);
        TrackEvent e = at_start;
        e.tick = start;
        e.after = 0;
        events.insert(at, e);
    }
}

std::optional<Performer::TickTime> Performer::NextTick() const
{
    // The build of October 2006 runs the next tick of the current frame, or the first tick of a later frame. Its time
    // is the frame's start. A tempo of 0 stops the count. The count is 16 bits wide and can wrap. The search still ends
    // within 2^16 frames.
    if (build_.ax_2006)
    {
        TickTime next{frame_,
                      0.0f,
                      static_cast<double>(frame_) * kFrameMs,
                      tick_units_,
                      frame_ticks_left_,
                      nominal_ms_ + MidiMsPerTick()};
        if (next.ticks_left > 0)
        {
            next.ticks_left--;
            return next;
        }

        for (int frames = 0; frames <= 0x10000 && (tempo_ > 0 || next.units >= kTickUnits); frames++)
        {
            next.frame++;
            const int ticks = StartFrameOctober2006(next.units, tempo_);
            if (ticks > 0)
            {
                next.ticks_left = ticks - 1;
                next.ms = static_cast<double>(next.frame) * kFrameMs;
                return next;
            }
        }

        return std::nullopt;
    }

    // SeqPlayer::UpdateTick (main.dol 0x8003eec4) runs once a sound frame for the frame's 3 ms. The first tick comes at
    // once and each later one 1 / (ticks per millisecond) after the one before; what's left of a frame passes to the
    // next as a fraction of a tick. All arithmetic is single precision.
    const float per_ms = TicksPerMs();
    if (per_ms == 0.0f)
    {
        return std::nullopt;
    }

    constexpr float kFrame = static_cast<float>(kFrameMs);
    TickTime next{frame_, rest_};
    float wait = 1.0f / per_ms;
    while (!(wait < next.rest))
    {
        wait -= next.rest;
        const float fraction = wait * per_ms;
        next.frame++;
        next.rest = kFrame;
        wait = fraction / per_ms;
    }

    next.rest -= wait;
    next.ms = static_cast<double>(next.frame) * kFrameMs + (kFrameMs - static_cast<double>(next.rest));

    return next;
}

void Performer::PlayOutStopped()
{
    // As in EndPerformance, finish one-shot waves with their per-frame pitch.
    UpdateOneShots(FrameOf(now_ms_ + kMaxTailMs) + 1);

    double end_ms = now_ms_;
    for (const Track& tr : tracks_)
    {
        for (const auto* list : {&tr.notes, &tr.detached})
        {
            for (const Note& n : *list)
            {
                out_.holds = out_.holds || n.EndMs() >= kNever;
                end_ms = std::max(end_ms, std::min(n.EndMs(), now_ms_ + kMaxTailMs));
            }
        }
    }
    if (out_.holds)
    {
        end_ms = now_ms_ + kHoldSeconds * 1000.0;
    }

    // MIDI ticks continue after sequence timing stops. Millisecond sweeps keep moving; note-length sweeps remain frozen
    // with the sequence's ticks (UpdateOutputs).
    const double ticks_per_second = out_.tempo.back().bpm * out_.timebase / 60.0;
    const auto ticks = static_cast<uint32_t>(std::ceil((end_ms - now_ms_) / 1000.0 * ticks_per_second));
    const double start_ms = now_ms_;
    for (uint32_t i = 1; i <= ticks; i++)
    {
        const double ms = start_ms + i / ticks_per_second * 1000.0;
        SweepBetweenTicks({FrameOf(ms), 0.0f, ms});
        tick_++;
        now_ms_ = ms;
        nominal_ms_ += 1000.0 / ticks_per_second;
        frame_ = FrameOf(now_ms_);
        for (int t = 0; t < 16; t++)
        {
            PruneNotes(t); // finished notes have no active sweep
            UpdateOutputs(t);
        }
    }
}

void Performer::EndPerformance(bool finished)
{
    // A main loop that holds a sound ends kHoldSeconds after its last change, without loop markers. Nothing changed in
    // the ticks after that.
    if (held_sound_end_ms_)
    {
        tick_ = std::min(MsToTick(*held_sound_end_ms_), tick_);
        now_ms_ = tick_ms_[tick_];
        out_.holds = true;
        out_.loop.reset();
        out_.loops = 0;
    }

    // Stop immediately at a loop boundary, time limit or held-sound limit. A naturally finished sequence ends with its
    // final sound, including release tails; NW4R SND can leave finished tracks waiting long after silence. Continue
    // one-shot waves with their per-frame pitch.
    if (finished)
    {
        UpdateOneShots(FrameOf(now_ms_ + kMaxTailMs) + 1);
    }

    for (int t = 0; t < 16; t++)
    {
        Track& tr = tracks_[t];
        for (auto* list : {&tr.notes, &tr.detached})
        {
            for (Note& n : *list)
            {
                // A note below hearing ends the sequence's sound where it has faded (Note::faded_ms). Stuck can end a
                // sequence there while its tracks wait out held notes.
                if (finished)
                {
                    const double end = BelowHearing(n) ? n.faded_ms : n.SoundEndMs();
                    sound_end_ms_ = std::max(sound_end_ms_, std::min(end, now_ms_ + kMaxTailMs));
                }
                MidiNoteOff(t, n);
            }
        }
    }

    // The tails' timed sweeps go on after the last tick, and their pitch events follow its note-offs.
    if (finished)
    {
        SweepAfterEnd();
    }
    for (Track& tr : tracks_)
    {
        tr.notes.clear();
        tr.detached.clear();
    }

    if (finished)
    {
        // After a main loop that sounded nothing (SilentLoop), what comes after the last sound can't be heard.
        if (silent_loop_)
        {
            const uint32_t end = MsToTick(sound_end_ms_);
            for (std::vector<TrackEvent>& events : out_.tracks)
            {
                std::erase_if(events, [end](const TrackEvent& e) { return e.tick > end; });
            }
            std::erase_if(out_.tempo, [end](const Performance::Tempo& t) { return t.tick > end; });
        }

        uint32_t last_event = 0;
        for (const auto& events : out_.tracks)
        {
            if (!events.empty())
            {
                last_event = std::max(last_event, events.back().tick);
            }
        }
        if (!out_.tempo.empty())
        {
            last_event = std::max(last_event, out_.tempo.back().tick);
        }

        out_.end_tick = std::max(last_event, MsToTick(sound_end_ms_));
        out_.seconds = std::max(sound_end_ms_, tick_ms_[last_event]) / 1000.0;
    }
    else
    {
        out_.end_tick = tick_;
        out_.seconds = now_ms_ / 1000.0;
    }
}

uint32_t Performer::MsToTick(double ms) const
{
    const auto it = std::lower_bound(tick_ms_.begin(), tick_ms_.end(), ms);
    if (it != tick_ms_.end())
    {
        return static_cast<uint32_t>(it - tick_ms_.begin());
    }

    // Extrapolate at the last tempo, or the MIDI tempo if sequence timing stopped.
    const double per_tick = MsPerTick() > 0 ? MsPerTick() : 60000.0 / (out_.tempo.back().bpm * out_.timebase);
    return static_cast<uint32_t>(tick_ms_.size() - 1 + std::ceil((ms - tick_ms_.back()) / per_tick));
}

// Scale factor that keeps rounded CC11 values within 127 for a maximum unscaled value of `peak` (Perform).
double LevelScale(double peak)
{
    return peak >= 127.5 ? 127.0 / peak : 1.0;
}

// Fit the parts that each track of `perf` used (Performer::PartsUsed) into MIDI's 16 channels. Give them out as the
// MIDI writer does: a channel for the first part of each track that plays notes, then the other parts, track by track.
// Return the parts each track may have.
std::array<uint8_t, 16> FitParts(const Performance& perf, const std::array<uint8_t, 16>& used)
{
    std::array<bool, 16> plays{};
    int channels = 0;
    for (int t = 0; t < 16; t++)
    {
        auto note_on = [](const TrackEvent& e)
        {
            return e.kind == EventKind::kNoteOn;
        };
        plays[t] = std::ranges::any_of(perf.tracks[t], note_on);
        channels += plays[t] ? 1 : 0;
    }

    std::array<uint8_t, 16> limits;
    limits.fill(1);
    for (int t = 0; t < 16; t++)
    {
        if (plays[t])
        {
            const int extra = std::min(std::max<int>(used[t], 1) - 1, 16 - channels);
            limits[t] = static_cast<uint8_t>(1 + extra);
            channels += extra;
        }
    }

    return limits;
}

} // namespace

EnvelopeValues OverrideEnvelope(EnvelopeValues env, const PresetKey& key)
{
    if (key.attack <= 127)
    {
        env.attack = key.attack;
    }
    if (key.decay <= 127)
    {
        env.decay = key.decay;
    }
    if (key.sustain <= 127)
    {
        env.sustain = key.sustain;
    }
    if (key.release <= 127)
    {
        env.release = key.release;
    }
    if (key.hold <= 127)
    {
        env.hold = key.hold;
    }

    return env;
}

EnvelopeValues RegionEnvelope(const Region& region, bool since_2008)
{
    return {region.attack, region.decay, region.sustain, since_2008 ? region.hold : 0, region.release};
}

Performance Perform(const SoundArchive& archive, uint32_t sound_index, BankSet& banks, const PerformOptions& options)
{
    const SoundInfo& sound = archive.Sounds()[sound_index];
    const auto file = archive.FileData(sound.file_id);
    if (file.empty() && !archive.FileMissing(sound.file_id))
    {
        throw FormatError("sequence file is not stored in the archive");
    }
    if (file.empty())
    {
        throw FormatError(archive.Truncated() ? "sequence extends past the truncated archive"
                                              : "sequence extends past the archive");
    }

    const Sequence seq = Sequence::Parse(file);
    const SndBuild build = options.build.value_or(BuildOfVersion(archive.Version()));

    // Rerun sequences in three cases. When filters change during notes, the rerun moves those filters from presets to
    // controllers. When legatos raise MIDI notes above the velocities they start at, the rerun starts those notes
    // higher (Rises). When rounded CC11 exceeds 127, the rerun scales every value equally, and the tracks keep their
    // balance.
    //
    // Only MIDI events and presets change between passes, but each change can move the others. With a controller-driven
    // filter, a legato may continue its MIDI note (Legato). That numbers the notes differently and changes the peak
    // CC11. A higher start lowers the peak. Note ids stay valid only between passes with the same filters. So run
    // passes until one leaves nothing to change, four at most. A pass whose parts don't fit MIDI's channels also
    // requires another pass. That pass keeps each track to the parts that fit (FitParts). Every later pass does the
    // same.
    std::array<bool, 16> filters{};
    std::map<uint32_t, int> starts;
    std::map<uint32_t, int16_t> attacks;
    double peak = 0.0;    // the CC11 peak the next pass scales for
    double applied = 0.0; // the peak the last pass scaled for
    std::array<uint8_t, 16> part_limits;
    part_limits.fill(16);
    bool limited = false;
    std::vector<Performer::LoopChoice> loop_choices;
    Performance performance;
    for (int pass = 0; pass < 4; pass++)
    {
        Performer performer(sound, seq, banks, options, build, filters, starts, attacks, LevelScale(peak), part_limits,
                            loop_choices);
        performance = performer.Run();
        applied = peak;
        loop_choices = performer.LoopChoices();

        bool parts_fit = true;
        if (!limited)
        {
            const std::array<uint8_t, 16> fitted = FitParts(performance, performer.PartsUsed());
            for (int t = 0; t < 16; t++)
            {
                parts_fit = parts_fit && fitted[t] >= performer.PartsUsed()[t];
            }
            if (!parts_fit)
            {
                part_limits = fitted;
                limited = true;
            }
        }

        const bool same_filters = performer.FilterMoves() == filters;
        const std::map<uint32_t, int> rises = same_filters ? performer.Rises() : std::map<uint32_t, int>{};
        const std::map<uint32_t, int16_t> early =
            same_filters ? performer.EarlyReleases() : std::map<uint32_t, int16_t>{};
        if (parts_fit && same_filters && rises == starts && early == attacks &&
            LevelScale(performer.LevelPeak()) == LevelScale(peak))
        {
            break;
        }

        filters = performer.FilterMoves();
        starts = rises;
        attacks = early;
        peak = performer.LevelPeak();
    }

    if (LevelScale(applied) < 1.0)
    {
        // CC11 acts squared. Scaling it by 127 / peak makes every note 40 log10(peak / 127) dB quieter.
        performance.level_cut_db = 40.0 * std::log10(applied / 127.0);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "volume exceeds MIDI range (SoundFont playback attenuated by %.1f dB; "
                      "SFZ restores the gain)",
                      performance.level_cut_db);
        performance.approximations[what] = NoteCount(performance);
    }

    return performance;
}

uint32_t NoteCount(const Performance& perf)
{
    uint32_t notes = 0;
    for (const std::vector<TrackEvent>& events : perf.tracks)
    {
        for (const TrackEvent& e : events)
        {
            if (e.kind == EventKind::kNoteOn)
            {
                notes++;
            }
        }
    }

    return notes;
}

} // namespace revosf2
