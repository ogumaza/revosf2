// SPDX-License-Identifier: MIT

// Unit tests run without game data. Pass a .brsar to also convert and validate every sequence in that archive.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "binary.h"
#include "convert.h"
#include "envelope.h"
#include "filter.h"
#include "instruments.h"
#include "midi.h"
#include "pan.h"
#include "performance.h"
#include "pitch.h"
#include "rsar.h"
#include "sf2.h"
#include "sfz.h"
#include "snd_build.h"
#include "wave.h"

using namespace revosf2;

namespace
{

// Failed check count.
int failures = 0;

// A tick at the default tempo and timebase, 120 beats a minute of 48 ticks, in milliseconds.
constexpr double kTickMs = 60000.0 / (120.0 * 48.0);

// Report and count a failed check, including file and line, without aborting the test.
#define REVOSF2_CHECK(cond)                                                               \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

uint32_t Le32(const std::vector<uint8_t>& d, std::size_t o)
{
    return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (static_cast<uint32_t>(d[o + 3]) << 24);
}

uint16_t Le16(const std::vector<uint8_t>& d, std::size_t o)
{
    return static_cast<uint16_t>(d[o] | (d[o + 1] << 8));
}

uint32_t Be32(const std::vector<uint8_t>& d, std::size_t o)
{
    return (static_cast<uint32_t>(d[o]) << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3];
}

// Offsets and sizes of SoundFont list subchunks.
struct Sf2View
{
    bool ok = false;
    std::map<std::string, std::pair<std::size_t, uint32_t>> chunks;
};

// Find SoundFont list subchunks; set `ok` false for invalid RIFF structure.
Sf2View ParseSf2(const std::vector<uint8_t>& d)
{
    Sf2View v;
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "sfbk", 4) != 0 ||
        Le32(d, 4) + 8 != d.size())
    {
        return v;
    }

    std::size_t off = 12;
    while (off + 8 <= d.size())
    {
        const uint32_t size = Le32(d, off + 4);
        if (std::memcmp(d.data() + off, "LIST", 4) != 0 || off + 8 + size > d.size())
        {
            return v;
        }

        std::size_t sub = off + 12;
        const std::size_t end = off + 8 + size;
        while (sub + 8 <= end)
        {
            const uint32_t s = Le32(d, sub + 4);
            v.chunks[std::string(reinterpret_cast<const char*>(d.data() + sub), 4)] = {sub + 8, s};
            sub += 8 + s + (s & 1);
        }
        if (sub != end)
        {
            return v;
        }

        off = end;
    }

    v.ok = off == d.size();

    return v;
}

// Like check_sf2.py, validate the SoundFont structures used by synths. Return an error description, or an empty string
// on success.
std::string Sf2Problem(const std::vector<uint8_t>& d)
{
    const Sf2View v = ParseSf2(d);
    if (!v.ok)
    {
        return "RIFF layout";
    }
    for (const char* c :
         {"ifil", "isng", "INAM", "smpl", "phdr", "pbag", "pmod", "pgen", "inst", "ibag", "imod", "igen", "shdr"})
    {
        if (!v.chunks.count(c))
        {
            return std::string("missing ") + c;
        }
    }

    const auto [shdr, shdr_size] = v.chunks.at("shdr");
    const auto [igen, igen_size] = v.chunks.at("igen");
    const auto [ibag, ibag_size] = v.chunks.at("ibag");
    const auto [inst, inst_size] = v.chunks.at("inst");
    const uint32_t smpl_samples = v.chunks.at("smpl").second / 2;
    if (shdr_size % 46 || igen_size % 4 || ibag_size % 4 || inst_size % 22)
    {
        return "record sizes";
    }

    const uint32_t nsamples = shdr_size / 46 - 1;
    for (uint32_t i = 0; i < nsamples; i++)
    {
        const std::size_t o = shdr + i * 46;
        const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
        const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
        if (!(start < end && end + 46 <= smpl_samples))
        {
            return "sample bounds";
        }
        if (le > ls && (ls < start || le > end))
        {
            return "loop bounds";
        }

        const uint16_t type = Le16(d, o + 44), link = Le16(d, o + 42);
        if (type == 2 || type == 4)
        {
            if (link >= nsamples || Le16(d, shdr + link * 46 + 42) != i || Le16(d, shdr + link * 46 + 44) != 6 - type)
            {
                return "stereo link";
            }
        }
        else if (type != 1)
        {
            return "sample type";
        }
    }

    const uint32_t ninst = inst_size / 22 - 1;
    for (uint32_t i = 0; i < ninst; i++)
    {
        const uint16_t b0 = Le16(d, inst + i * 22 + 20), b1 = Le16(d, inst + (i + 1) * 22 + 20);
        for (uint16_t b = b0; b < b1; b++)
        {
            const uint16_t g0 = Le16(d, ibag + b * 4), g1 = Le16(d, ibag + (b + 1) * 4);
            std::map<uint16_t, uint16_t> gens;
            for (uint16_t g = g0; g < g1; g++)
            {
                const uint16_t oper = Le16(d, igen + g * 4);
                if (oper == 43 && g != g0)
                {
                    return "keyRange not first";
                }
                if (oper == 44 && g != g0 + (Le16(d, igen + g0 * 4) == 43 ? 1 : 0))
                {
                    return "velRange out of place";
                }
                if (oper == 53 && g != g1 - 1)
                {
                    return "sampleID not last";
                }

                gens[oper] = Le16(d, igen + g * 4 + 2);
            }

            if (gens.count(53))
            {
                if (gens[53] >= nsamples)
                {
                    return "sampleID out of range";
                }

                if (gens.count(54) && (gens[54] & 1))
                {
                    const std::size_t o = shdr + gens[53] * 46;
                    const uint32_t start = Le32(d, o + 20), end = Le32(d, o + 24);
                    const uint32_t ls = Le32(d, o + 28), le = Le32(d, o + 32);
                    if (ls - start < 8 || end - le < 8 || le - ls < 32)
                    {
                        return "loop rules";
                    }
                }
            }
        }
    }

    return "";
}

// A parsed MIDI note: track, channel, key and start/end ticks.
struct MidiNote
{
    int track = 0, channel = 0, key = 0;
    uint32_t on = 0, off = 0;
};

// Parsed MIDI track names, end ticks, notes, pitch bends and markers.
struct MidiView
{
    bool ok = false;
    uint16_t division = 0;
    std::vector<std::string> track_names;
    std::vector<uint64_t> track_ends;
    std::vector<MidiNote> notes;
    std::vector<std::pair<uint32_t, int>> bends; // tick and 14-bit value
    std::vector<std::pair<uint32_t, std::string>> markers;
};

// Read generated MIDI. Set `ok` false for malformed data, deltas over four bytes, running status or overlapping
// same-key notes on one channel.
MidiView ParseMidi(const std::vector<uint8_t>& d)
{
    MidiView v;
    if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4) != 0 || Be32(d, 4) != 6)
    {
        return v;
    }

    const uint16_t ntracks = static_cast<uint16_t>((d[10] << 8) | d[11]);
    v.division = static_cast<uint16_t>((d[12] << 8) | d[13]);
    std::size_t off = 14;
    for (uint16_t t = 0; t < ntracks; t++)
    {
        if (off + 8 > d.size() || std::memcmp(d.data() + off, "MTrk", 4) != 0)
        {
            return v;
        }

        const uint32_t size = Be32(d, off + 4);
        std::size_t p = off + 8;
        const std::size_t end = p + size;
        if (end > d.size())
        {
            return v;
        }

        uint64_t tick = 0;
        std::map<std::pair<int, int>, uint32_t> on;
        std::string name;
        bool ended = false;
        while (p < end)
        {
            uint32_t delta = 0;
            uint8_t b;
            int length = 0;
            do
            {
                b = d[p++];
                delta = (delta << 7) | (b & 0x7f);
            } while ((b & 0x80) && ++length < 4);
            if (b & 0x80)
            {
                return v; // a delta time of more than 4 bytes
            }

            tick += delta;

            const uint8_t status = d[p++];
            if (status == 0xff)
            {
                const uint8_t type = d[p++];
                uint32_t len = 0;
                do
                {
                    b = d[p++];
                    len = (len << 7) | (b & 0x7f);
                } while (b & 0x80);

                const std::string text(reinterpret_cast<const char*>(d.data() + p), len);
                if (type == 0x03)
                {
                    name = text;
                }
                if (type == 0x06)
                {
                    v.markers.push_back({static_cast<uint32_t>(tick), text});
                }
                if (type == 0x2f)
                {
                    ended = true;
                }

                p += len;
                continue;
            }

            const int kind = status >> 4, ch = status & 15;
            if (kind < 8)
            {
                return v; // no running status is written
            }

            const int nbytes = (kind == 0xc || kind == 0xd) ? 1 : 2;
            const uint8_t a = d[p], bb = nbytes == 2 ? d[p + 1] : 0;
            p += nbytes;
            if (kind == 9 && bb > 0)
            {
                if (on.count({ch, a}))
                {
                    return v; // a key on twice on one channel
                }

                on[{ch, a}] = static_cast<uint32_t>(tick);
            }
            else if (kind == 8 || (kind == 9 && bb == 0))
            {
                auto it = on.find({ch, a});
                if (it != on.end())
                {
                    v.notes.push_back({t, ch, a, it->second, static_cast<uint32_t>(tick)});
                    on.erase(it);
                }
            }
            else if (kind == 0xe)
            {
                v.bends.push_back({static_cast<uint32_t>(tick), a | (bb << 7)});
            }
        }
        if (!ended || p != end || !on.empty())
        {
            return v;
        }

        v.track_names.push_back(name);
        v.track_ends.push_back(tick);
        off = end;
    }

    v.ok = off == d.size();

    return v;
}

// Convert a string to bytes.
std::vector<uint8_t> Bytes(const std::string& s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Convert bytes to a string for comparison.
std::string Text(std::span<const uint8_t> data)
{
    return std::string(data.begin(), data.end());
}

// Big-endian writer with a growing buffer, for the Wii's formats.
struct Writer
{
    void U8(std::size_t at, uint32_t v)
    {
        bytes[at] = static_cast<uint8_t>(v);
    }

    void U16(std::size_t at, uint32_t v)
    {
        bytes[at] = static_cast<uint8_t>(v >> 8);
        bytes[at + 1] = static_cast<uint8_t>(v);
    }

    void U32(std::size_t at, uint32_t v)
    {
        U16(at, v >> 16);
        U16(at + 2, v & 0xffff);
    }

    // Write an offset reference: an offset (type 1) with its data type.
    void Ref(std::size_t at, uint8_t data_type, std::size_t offset)
    {
        U8(at, 1);
        U8(at + 1, data_type);
        U32(at + 4, static_cast<uint32_t>(offset));
    }

    // Append `n` zero bytes and return their offset.
    std::size_t Add(std::size_t n)
    {
        bytes.resize(bytes.size() + n);

        return bytes.size() - n;
    }

    // Append `data` and return its offset.
    std::size_t Add(std::span<const uint8_t> data)
    {
        const std::size_t at = Add(data.size());
        std::copy(data.begin(), data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));

        return at;
    }

    std::vector<uint8_t> bytes;
};

// Build a file header with `magic`, `version` and space for `blocks` block entries after it, at 0x10. The caller fills
// them and the file size.
Writer FileHeader(const char* magic, uint16_t version, int blocks)
{
    Writer w;
    w.Add(0x20);
    std::memcpy(w.bytes.data(), magic, 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, version);
    w.U16(0x0c, 0x20);
    w.U16(0x0e, static_cast<uint32_t>(blocks));

    return w;
}

// Start a block with `magic` and return its offset. Block finishes it.
std::size_t StartBlock(Writer& w, const char* magic)
{
    const std::size_t at = w.Add(8);
    std::memcpy(w.bytes.data() + at, magic, 4);

    return at;
}

// Finish the block at `at` as block `i` of the header's table, and the file's size.
void Block(Writer& w, int i, std::size_t at)
{
    w.U32(at + 4, static_cast<uint32_t>(w.bytes.size() - at));
    w.U32(0x10 + i * 8, static_cast<uint32_t>(at));
    w.U32(0x14 + i * 8, static_cast<uint32_t>(w.bytes.size() - at));
    w.U32(0x08, static_cast<uint32_t>(w.bytes.size()));
}

// Write a PCM16 wave info at `info` for `length` samples at `sample_rate`, looping from 0 if `loops`, with `channels`
// channels whose samples start at `samples_at` from the wave data's start.
void WaveInfo(Writer& w, std::size_t info, uint32_t length, uint32_t channels, bool loops, uint32_t sample_rate,
              uint32_t data_offset, uint32_t samples_at)
{
    w.U8(info, 1); // PCM16
    w.U8(info + 1, loops ? 1 : 0);
    w.U8(info + 2, channels);
    w.U8(info + 3, sample_rate >> 16);
    w.U16(info + 4, sample_rate & 0xffff);
    w.U32(info + 0xC, length - 1); // the last sample's address
    w.U32(info + 0x10, 0x1C);
    w.U32(info + 0x14, data_offset);
    for (uint32_t c = 0; c < channels; c++)
    {
        w.U32(info + 0x1C + c * 4, 0x1C + channels * 4 + c * 0x18);
        w.U32(info + 0x1C + channels * 4 + c * 0x18, samples_at);
    }
}

// The size of a wave info with `channels` channels.
std::size_t WaveInfoSize(uint32_t channels)
{
    return 0x1C + channels * (4 + 0x18);
}

// Append `samples` as big-endian PCM16 and return their offset.
std::size_t AddSamples(Writer& w, const std::vector<int16_t>& samples)
{
    const std::size_t at = w.Add(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); i++)
    {
        w.U16(at + i * 2, static_cast<uint16_t>(samples[i]));
    }

    return at;
}

// Build a PCM16 RWAV declaring `length` samples at `sample_rate`. Each of `channels` channels uses `samples`. If
// `loops`, loop the entire wave.
std::vector<uint8_t> Rwav(uint32_t length, const std::vector<int16_t>& samples, uint32_t channels = 1,
                          bool loops = false, uint32_t sample_rate = 32000)
{
    Writer w = FileHeader("RWAV", 0x0102, 2);
    const std::size_t info = StartBlock(w, "INFO");
    w.Add(WaveInfoSize(channels));
    WaveInfo(w, info + 8, length, channels, loops, sample_rate, 0, 0);
    Block(w, 0, info);

    const std::size_t data = StartBlock(w, "DATA");
    AddSamples(w, samples);
    Block(w, 1, data);

    return w.bytes;
}

// Build an RWAR holding RWAV files `waves`.
std::vector<uint8_t> Rwar(const std::vector<std::vector<uint8_t>>& waves)
{
    Writer w = FileHeader("RWAR", 0x0100, 2);
    const std::size_t table = StartBlock(w, "TABL");
    w.Add(4 + waves.size() * 12);
    w.U32(table + 8, static_cast<uint32_t>(waves.size()));
    Block(w, 0, table);

    const std::size_t data = StartBlock(w, "DATA");
    for (std::size_t i = 0; i < waves.size(); i++)
    {
        w.Ref(table + 0xC + i * 12, 0, w.Add(waves[i]) - data);
        w.U32(table + 0xC + i * 12 + 8, static_cast<uint32_t>(waves[i].size()));
    }
    Block(w, 1, data);

    return w.bytes;
}

// A test bank region.
struct TestRegion
{
    int32_t wave_index = 0;
    uint8_t attack = 127, decay = 127, sustain = 127, release = 127, hold = 0, note_off_type = 0, key_group = 0;
    uint8_t original_key = 60, volume = 127, pan = 64;
    float tune = 1.0f;
};

// A test instrument: a region for every key and velocity, or a range table of `upper_keys` or `upper_velocities`, one
// region for each.
struct TestInstrument
{
    std::vector<TestRegion> regions = {TestRegion{}};
    std::vector<uint8_t> upper_keys = {};
    std::vector<uint8_t> upper_velocities = {};
};

// Write `r` at `at`.
void WriteRegion(Writer& w, std::size_t at, const TestRegion& r)
{
    w.U32(at, static_cast<uint32_t>(r.wave_index));
    const uint8_t kFields[] = {r.attack,        r.decay,     r.sustain,      r.release, r.hold, 0,
                               r.note_off_type, r.key_group, r.original_key, r.volume,  r.pan,  0};
    for (std::size_t i = 0; i < std::size(kFields); i++)
    {
        w.U8(at + 4 + i, kFields[i]);
    }

    uint32_t tune;
    std::memcpy(&tune, &r.tune, 4);
    w.U32(at + 0x10, tune);
}

// Build an RBNK of `instruments`. A version 1.1 bank also gets a WAVE block describing `waves` PCM16 waves of `length`
// samples each, whose samples follow one another in the bank's wave data; a later bank's waves are an RWAR.
std::vector<uint8_t> Rbnk(const std::vector<TestInstrument>& instruments, uint16_t version = 0x0102, uint32_t waves = 0,
                          uint32_t length = 0)
{
    Writer w = FileHeader("RBNK", version, waves ? 2 : 1);
    const std::size_t data = StartBlock(w, "DATA"), base = data + 8;
    const std::size_t table = w.Add(4 + instruments.size() * 8);
    w.U32(table, static_cast<uint32_t>(instruments.size()));
    for (std::size_t i = 0; i < instruments.size(); i++)
    {
        const TestInstrument& inst = instruments[i];
        if (inst.upper_keys.empty() && inst.upper_velocities.empty())
        {
            const std::size_t region = w.Add(0x14);
            WriteRegion(w, region, inst.regions[0]);
            w.Ref(table + 4 + i * 8, 1, region - base);
            continue;
        }

        // A range table: a count and each entry's upper key or velocity, then a reference to each entry's region.
        auto range_table = [&](const std::vector<uint8_t>& uppers)
        {
            const std::size_t n = uppers.size(), refs = (n + 1 + 3) & ~std::size_t{3};
            const std::size_t range = w.Add(refs + n * 8);
            w.U8(range, static_cast<uint32_t>(n));
            for (std::size_t k = 0; k < n; k++)
            {
                w.U8(range + 1 + k, uppers[k]);
                const std::size_t region = w.Add(0x14);
                WriteRegion(w, region, inst.regions[k]);
                w.Ref(range + refs + k * 8, 1, region - base);
            }
            return range;
        };

        if (inst.upper_velocities.empty())
        {
            w.Ref(table + 4 + i * 8, 2, range_table(inst.upper_keys) - base);
            continue;
        }

        // Velocities split under a key table of one entry, for every key.
        const std::size_t keys = w.Add(4 + 8);
        w.U8(keys, 1);
        w.U8(keys + 1, 127);
        w.Ref(keys + 4, 2, range_table(inst.upper_velocities) - base);
        w.Ref(table + 4 + i * 8, 2, keys - base);
    }
    Block(w, 0, data);

    if (waves)
    {
        const std::size_t wave = StartBlock(w, "WAVE"), wave_base = wave + 8;
        const std::size_t refs = w.Add(4 + waves * 8);
        w.U32(refs, waves);
        for (uint32_t i = 0; i < waves; i++)
        {
            const std::size_t info = w.Add(WaveInfoSize(1));
            WaveInfo(w, info, length, 1, false, 32000, i * length * 2, 0);
            w.Ref(refs + 4 + i * 8, 0, info - wave_base);
        }
        Block(w, 1, wave);
    }

    return w.bytes;
}

// Build an RSEQ with `commands` in its DATA block.
std::vector<uint8_t> Rseq(const std::vector<uint8_t>& commands)
{
    Writer w = FileHeader("RSEQ", 0x0100, 1);
    const std::size_t data = StartBlock(w, "DATA");
    w.Add(4);
    w.U32(data + 8, 0xC);
    w.Add(commands);
    Block(w, 0, data);

    return w.bytes;
}

// A test file and the wave data that goes with it.
struct ArchiveFile
{
    std::vector<uint8_t> data;
    std::vector<uint8_t> wave_data = {};
};

// A test sequence sound.
struct SequenceSound
{
    uint32_t file_id = 0;
    uint32_t start_offset = 0;
    uint32_t bank = 0;
    uint8_t pan_mode = 0, pan_curve = 0;
    uint8_t volume = 127;
    uint32_t allocate_track_flags = 1;
};

// Test archive tables: the archive's version, its sequences and banks (by file ID), its sounds' names, and the groups
// that hold its files (by file ID; by default one group holds them all). Defaults allow partial designated initializers
// without warnings.
struct ArchiveLists
{
    uint16_t version = 0x0104;
    std::vector<SequenceSound> sequences = {};
    std::vector<uint32_t> banks = {};
    std::vector<std::string> names = {};
    std::vector<std::vector<uint32_t>> groups = {};
};

// Build a sound archive of `files`, with metadata from `lists`. Each group holds its files and then their wave data.
std::vector<uint8_t> Rsar(const std::vector<ArchiveFile>& files, const ArchiveLists& lists = {})
{
    Writer w;
    w.Add(0x40);
    std::memcpy(w.bytes.data(), "RSAR", 4);
    w.U16(0x04, 0xfeff);
    w.U16(0x06, lists.version);
    w.U16(0x0c, 0x40);
    w.U16(0x0e, 3);

    // SYMB: an offset, from the block's payload, to a table of offsets, from the payload too, to the names.
    const std::size_t symb = StartBlock(w, "SYMB"), symb_base = symb + 8;
    w.Add(4);
    const std::size_t strings = w.Add(4 + lists.names.size() * 4);
    w.U32(symb_base, static_cast<uint32_t>(strings - symb_base));
    w.U32(strings, static_cast<uint32_t>(lists.names.size()));
    for (std::size_t i = 0; i < lists.names.size(); i++)
    {
        const std::size_t at = w.Add(lists.names[i].size() + 1);
        std::memcpy(w.bytes.data() + at, lists.names[i].data(), lists.names[i].size());
        w.U32(strings + 4 + i * 4, static_cast<uint32_t>(at - symb_base));
    }
    w.U32(symb + 4, static_cast<uint32_t>(w.bytes.size() - symb));
    w.U32(0x10, static_cast<uint32_t>(symb));
    w.U32(0x14, static_cast<uint32_t>(w.bytes.size() - symb));

    // INFO: references to the sound, bank, player, file and group tables.
    const std::size_t info = StartBlock(w, "INFO"), base = info + 8;
    w.Add(6 * 8);
    auto table = [&](int i, std::size_t count)
    {
        const std::size_t t = w.Add(4 + count * 8);
        w.U32(t, static_cast<uint32_t>(count));
        w.Ref(base + i * 8, 0, t - base);
        return t;
    };

    const std::size_t sounds = table(0, lists.sequences.size());
    for (std::size_t i = 0; i < lists.sequences.size(); i++)
    {
        const SequenceSound& s = lists.sequences[i];
        const std::size_t e = w.Add(0x2C), d = w.Add(0x10);
        w.Ref(sounds + 4 + i * 8, 0, e - base);
        w.U32(e, i < lists.names.size() ? static_cast<uint32_t>(i) : 0xffffffff);
        w.U32(e + 4, s.file_id);
        w.U8(e + 0x14, s.volume);
        w.U8(e + 0x16, 1); // a sequence
        w.Ref(e + 0x18, 1, d - base);
        w.U8(e + 0x28, s.pan_mode);
        w.U8(e + 0x29, s.pan_curve);
        w.U32(d, s.start_offset);
        w.U32(d + 4, s.bank);
        w.U32(d + 8, s.allocate_track_flags);
        w.U8(d + 0xC, 64);
    }

    const std::size_t banks = table(1, lists.banks.size());
    for (std::size_t i = 0; i < lists.banks.size(); i++)
    {
        const std::size_t e = w.Add(0xC);
        w.Ref(banks + 4 + i * 8, 0, e - base);
        w.U32(e, 0xffffffff);
        w.U32(e + 4, lists.banks[i]);
    }

    table(2, 0);

    std::vector<std::vector<uint32_t>> groups = lists.groups;
    if (groups.empty())
    {
        groups.emplace_back();
        for (uint32_t id = 0; id < files.size(); id++)
        {
            groups[0].push_back(id);
        }
    }

    // Each file's positions: the groups that hold it, and its item in each.
    const std::size_t file_table = table(3, files.size());
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> positions(files.size());
    for (uint32_t g = 0; g < groups.size(); g++)
    {
        for (uint32_t k = 0; k < groups[g].size(); k++)
        {
            positions[groups[g][k]].emplace_back(g, k);
        }
    }
    for (std::size_t id = 0; id < files.size(); id++)
    {
        const std::size_t e = w.Add(0x1C);
        w.Ref(file_table + 4 + id * 8, 0, e - base);
        const std::size_t places = w.Add(4 + positions[id].size() * 8);
        w.U32(places, static_cast<uint32_t>(positions[id].size()));
        w.Ref(e + 0x14, 0, places - base);
        for (std::size_t p = 0; p < positions[id].size(); p++)
        {
            const std::size_t position = w.Add(8);
            w.U32(position, positions[id][p].first);
            w.U32(position + 4, positions[id][p].second);
            w.Ref(places + 4 + p * 8, 0, position - base);
        }
    }

    const std::size_t group_table = table(4, groups.size());
    std::vector<std::size_t> group_entries, item_entries;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = w.Add(0x28);
        w.Ref(group_table + 4 + g * 8, 0, e - base);
        group_entries.push_back(e);
        const std::size_t items = w.Add(4 + groups[g].size() * 8);
        w.U32(items, static_cast<uint32_t>(groups[g].size()));
        w.Ref(e + 0x20, 0, items - base);
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = w.Add(0x18);
            w.U32(item, groups[g][k]);
            w.Ref(items + 4 + k * 8, 0, item - base);
            item_entries.push_back(item);
        }
    }
    table(5, 0);
    w.U32(info + 4, static_cast<uint32_t>(w.bytes.size() - info));
    w.U32(0x18, static_cast<uint32_t>(info));
    w.U32(0x1c, static_cast<uint32_t>(w.bytes.size() - info));

    // FILE: each group's files, then their wave data.
    const std::size_t file_block = StartBlock(w, "FILE");
    std::size_t item_index = 0;
    for (std::size_t g = 0; g < groups.size(); g++)
    {
        const std::size_t e = group_entries[g];
        const std::size_t start = w.bytes.size();
        std::vector<std::size_t> offsets;
        for (const uint32_t id : groups[g])
        {
            offsets.push_back(w.Add(files[id].data) - start);
        }
        const std::size_t wave_start = w.bytes.size();
        std::vector<std::size_t> wave_offsets;
        for (const uint32_t id : groups[g])
        {
            wave_offsets.push_back(w.Add(files[id].wave_data) - wave_start);
        }

        w.U32(e + 0x10, static_cast<uint32_t>(start));
        w.U32(e + 0x14, static_cast<uint32_t>(wave_start - start));
        w.U32(e + 0x18, static_cast<uint32_t>(wave_start));
        w.U32(e + 0x1C, static_cast<uint32_t>(w.bytes.size() - wave_start));
        for (std::size_t k = 0; k < groups[g].size(); k++)
        {
            const std::size_t item = item_entries[item_index++];
            const ArchiveFile& f = files[groups[g][k]];
            w.U32(item + 4, static_cast<uint32_t>(offsets[k]));
            w.U32(item + 8, static_cast<uint32_t>(f.data.size()));
            w.U32(item + 0xC, static_cast<uint32_t>(wave_offsets[k]));
            w.U32(item + 0x10, static_cast<uint32_t>(f.wave_data.size()));
        }
    }
    w.U32(file_block + 4, static_cast<uint32_t>(w.bytes.size() - file_block));
    w.U32(0x20, static_cast<uint32_t>(file_block));
    w.U32(0x24, static_cast<uint32_t>(w.bytes.size() - file_block));
    w.U32(0x08, static_cast<uint32_t>(w.bytes.size()));

    return w.bytes;
}

// The files of a test archive whose one sequence, file 0, plays `commands` on a bank, file 1, with one region that
// plays a wave, of an RWAR in its wave data, of 64 samples of 1000 that loops.
std::vector<ArchiveFile> LoopingWaveFiles(const std::vector<uint8_t>& commands, TestRegion region = {})
{
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq(commands);
    files[1].data = Rbnk({TestInstrument{{region}}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)});

    return files;
}

// Perform the sound `index` of `archive`. If `build` is given, play as that build of NW4R SND does (PerformOptions).
Performance PerformSound(const SoundArchive& archive, uint32_t index, std::optional<SndBuild> build = std::nullopt)
{
    BankSet banks(archive, archive.Sounds()[index]);
    PerformOptions options;
    options.build = build;

    return Perform(archive, index, banks, options);
}

// Options for SequenceArchive.
struct TestSounds
{
    uint16_t version = 0x0104;      // the archive's version: 1.4 plays as a build of NW4R SND from 2008
    uint32_t track_flags = 1;       // the tracks each sound allocates
    TestRegion region = {};         // the bank's one region
    std::vector<uint8_t> wave = {}; // the RWAV the region plays: 64 samples of 1000 that loop if empty

    // Regions split by velocity, up to each of `upper_velocities`, in place of `region`.
    std::vector<TestRegion> velocity_regions = {};
    std::vector<uint8_t> upper_velocities = {};

    // The build of NW4R SND that a main.dol shows, in place of the one that `version` gives.
    std::optional<SndBuild> build = std::nullopt;
};

// Build an archive with a sound for each of `sequences`, all of them on one bank whose one region plays one wave.
SoundArchive SequenceArchive(const std::vector<std::vector<uint8_t>>& sequences, const TestSounds& options = {})
{
    std::vector<ArchiveFile> files;
    std::vector<SequenceSound> sounds;
    for (const std::vector<uint8_t>& commands : sequences)
    {
        sounds.push_back({.file_id = static_cast<uint32_t>(files.size()), .allocate_track_flags = options.track_flags});
        files.push_back({Rseq(commands)});
    }

    const std::vector<uint8_t> wave =
        options.wave.empty() ? Rwav(64, std::vector<int16_t>(64, 1000), 1, true) : options.wave;
    const TestInstrument instrument =
        options.upper_velocities.empty()
            ? TestInstrument{{options.region}}
            : TestInstrument{.regions = options.velocity_regions, .upper_velocities = options.upper_velocities};
    files.push_back({Rbnk({instrument}), Rwar({wave})});

    const uint32_t bank = static_cast<uint32_t>(sequences.size());
    return SoundArchive::Load(Rsar(files, {.version = options.version, .sequences = sounds, .banks = {bank}}));
}

// Perform `commands` as the one sound of a SequenceArchive.
Performance PerformCommands(const std::vector<uint8_t>& commands, const TestSounds& options = {})
{
    return PerformSound(SequenceArchive({commands}, options), 0, options.build);
}

// The ticks and kinds of track `t`'s note events.
std::vector<std::pair<uint32_t, EventKind>> NoteEvents(const Performance& p, int t = 0)
{
    std::vector<std::pair<uint32_t, EventKind>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff)
        {
            notes.emplace_back(e.tick, e.kind);
        }
    }

    return notes;
}

// The ticks, keys and velocities of track `t`'s note-ons.
std::vector<std::tuple<uint32_t, int, int>> NoteOns(const Performance& p, int t = 0)
{
    std::vector<std::tuple<uint32_t, int, int>> notes;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            notes.emplace_back(e.tick, e.key, e.value);
        }
    }

    return notes;
}

// The ticks and values of track `t`'s events of `kind`.
std::vector<std::pair<uint32_t, int>> Controls(const Performance& p, EventKind kind, int t = 0)
{
    std::vector<std::pair<uint32_t, int>> values;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == kind)
        {
            values.emplace_back(e.tick, e.value);
        }
    }

    return values;
}

// The ticks and values of track `t`'s events of `kind` on `part`.
std::vector<std::pair<uint32_t, int>> PartControls(const Performance& p, EventKind kind, uint8_t part, int t = 0)
{
    std::vector<std::pair<uint32_t, int>> values;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == kind && e.part == part)
        {
            values.emplace_back(e.tick, e.value);
        }
    }

    return values;
}

// The parts of track `t`'s note-ons.
std::vector<int> NoteParts(const Performance& p, int t = 0)
{
    std::vector<int> parts;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            parts.push_back(e.part);
        }
    }

    return parts;
}

// The ticks and semitones of track `t`'s pitch events.
std::vector<std::pair<uint32_t, float>> Pitches(const Performance& p, int t = 0)
{
    std::vector<std::pair<uint32_t, float>> pitches;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kPitch)
        {
            pitches.emplace_back(e.tick, e.semitones);
        }
    }

    return pitches;
}

void TestDspAdpcmScalesNibbles()
{
    // Header 0x12: scale 1 << 2, coefficient pair 1. With zero coefficients a sample is nibble * scale.
    const int16_t kCoefs[16] = {};
    const std::vector<uint8_t> kFrame = {0x12, 0x12, 0x34, 0xf0, 0x00, 0x00, 0x00, 0x00};
    const int16_t kExpect[6] = {4, 8, 12, 16, -4, 0};

    const auto out = DecodeDspAdpcm(kFrame, 6, kCoefs, 0, 0);

    REVOSF2_CHECK(out.size() == 6);
    for (int i = 0; i < 6 && i < static_cast<int>(out.size()); i++)
    {
        REVOSF2_CHECK(out[i] == kExpect[i]);
    }
}

void TestDspAdpcmPredictsFromHistory()
{
    // Coefficient pair 0 = (2048, 0) repeats the previous sample.
    const int16_t kHold[16] = {2048, 0};

    const auto held = DecodeDspAdpcm(std::vector<uint8_t>{0x00, 0x00, 0, 0, 0, 0, 0, 0}, 3, kHold, 100, 0);

    REVOSF2_CHECK(held.size() == 3 && held[0] == 100 && held[2] == 100);
}

void TestDspAdpcmClampsLargePredictions()
{
    // Two coefficients and two history samples at -32768 sum to 2^31 before division by 2048. That overflows int32_t.
    const int16_t kCoefs[16] = {-32768, -32768};

    const auto out = DecodeDspAdpcm(std::vector<uint8_t>{0x00, 0x00, 0, 0, 0, 0, 0, 0}, 1, kCoefs, -32768, -32768);

    REVOSF2_CHECK(out.size() == 1 && out[0] == 32767);
}

void TestPcm16WaveDecodes()
{
    // The Wii's PCM16 is big-endian.
    const Pcm pcm = DecodeWave(Wave::ParseFile(Rwav(4, {1, -2, 300, -32768})));

    REVOSF2_CHECK(pcm.channels.size() == 1 && !pcm.loop && pcm.Length() == 4 && pcm.sample_rate == 32000);
    REVOSF2_CHECK(pcm.channels[0] == std::vector<int16_t>({1, -2, 300, -32768}));
}

void TestWaveLongerThanItsDataIsReported()
{
    // The header claims 2^32 - 1 samples; the data holds four.
    const std::vector<uint8_t> rwav = Rwav(0xffffffff, {1, 2, 3, 4});

    std::string message;
    try
    {
        DecodeWave(Wave::ParseFile(rwav));
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    REVOSF2_CHECK(message == "wave data shorter than the declared sample count");
}

void TestWaveDecodesItsFirstTwoChannels()
{
    // Ignore channels after the first two. NW4R SND ignores them too.
    const Pcm pcm = DecodeWave(Wave::ParseFile(Rwav(4, {1, -2, 300, -32768}, 3)));

    REVOSF2_CHECK(pcm.channels.size() == 2 && pcm.channels[1] == std::vector<int16_t>({1, -2, 300, -32768}));
}

void TestAdpcmLoopIsInSamples()
{
    // A DSP-ADPCM wave gives its loop as nibble addresses, two header nibbles to each 8-byte frame of 14 samples: the
    // loop start's address 18 is sample 14, the second frame's first, and the end's address 47 the last sample of a
    // 42-sample wave.
    Writer w = FileHeader("RWAV", 0x0102, 2);
    const std::size_t info = StartBlock(w, "INFO");
    w.Add(WaveInfoSize(1) + 0x30);
    WaveInfo(w, info + 8, 1, 1, true, 32000, 0, 0);
    w.U8(info + 8, 2);
    w.U32(info + 8 + 8, 18);
    w.U32(info + 8 + 0xC, 47);
    w.U32(info + 8 + 0x1C + 4 + 4, static_cast<uint32_t>(WaveInfoSize(1))); // the ADPCM info, after the wave info
    Block(w, 0, info);
    const std::size_t data = StartBlock(w, "DATA");
    w.Add(24);
    Block(w, 1, data);

    const Pcm pcm = DecodeWave(Wave::ParseFile(w.bytes));

    REVOSF2_CHECK(pcm.loop && pcm.loop_start == 14 && pcm.loop_end == 42 && pcm.channels[0].size() == 42);
}

void TestVersionOneBankDescribesItsWaves()
{
    // An RBNK 1.1 bank's WAVE block describes its waves, whose samples are the raw wave data: two four-sample waves.
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq({0xff});
    files[1].data = Rbnk({TestInstrument{{{.wave_index = 1}}}}, 0x0101, 2, 4);
    Writer samples;
    AddSamples(samples, {1, 2, 3, 4, -5, -6, -7, -8});
    files[1].wave_data = samples.bytes;
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.version = 0x0101, .sequences = {{}}, .banks = {1}}));
    BankSet banks(archive, archive.Sounds()[0]);

    const Pcm* second = banks.GetWave(1);

    REVOSF2_CHECK(banks.GetBank() && banks.GetBank()->HasWaveBlock() && banks.Errors().empty());
    REVOSF2_CHECK(second && second->channels[0] == std::vector<int16_t>({-5, -6, -7, -8}));
}

void TestBankSelectsRegionsAsTheGameDoes()
{
    // Program 0 splits at key 59 into regions playing waves 0 and 1. Program 1's only region has no wave (a negative
    // index) and plays nothing. Program 2 doesn't exist and plays nothing either.
    const std::vector<uint8_t> rbnk =
        Rbnk({TestInstrument{{{.wave_index = 0}, {.wave_index = 1}}, {59, 127}}, TestInstrument{{{.wave_index = -1}}}});

    const Bank bank = Bank::Parse(rbnk);

    REVOSF2_CHECK(bank.InstrumentCount() == 2);
    REVOSF2_CHECK(bank.Find(0, 59, 100) && bank.Find(0, 59, 100)->wave_index == 0);
    REVOSF2_CHECK(bank.Find(0, 60, 1) && bank.Find(0, 60, 1)->wave_index == 1);
    REVOSF2_CHECK(!bank.Find(1, 60, 100) && !bank.Find(2, 60, 100));

    // The SoundFont gets a zone for each region, with the keys and velocities that play it.
    const std::vector<PlayedRanges> ranges = RegionRanges(bank, 0);
    const std::vector<std::pair<int, int>> kAll = {{0, 127}};
    REVOSF2_CHECK(ranges.size() == 2 && ranges[0].keys == (std::vector<std::pair<int, int>>{{0, 59}}) &&
                  ranges[1].keys == (std::vector<std::pair<int, int>>{{60, 127}}) && ranges[1].velocities == kAll);
    REVOSF2_CHECK(RegionRanges(bank, 1).empty());
}

void TestOldBankHasNoVolumeOrTune()
{
    // BankFileReader::ReadInstInfo gives a region of a bank older than version 1.1 volume 127 and tune 1.
    const TestRegion kRegion{.volume = 64, .tune = 2.0f};

    const Bank old = Bank::Parse(Rbnk({TestInstrument{{kRegion}}}, 0x0100));
    const Bank current = Bank::Parse(Rbnk({TestInstrument{{kRegion}}}, 0x0101));

    REVOSF2_CHECK(old.Find(0, 60, 100)->volume == 127 && old.Find(0, 60, 100)->tune == 1.0f);
    REVOSF2_CHECK(current.Find(0, 60, 100)->volume == 64 && current.Find(0, 60, 100)->tune == 2.0f);
}

void TestFilesAreFoundInGroups()
{
    // Group 0 holds files 0 and 1, group 1 files 1 and 2; file 3 is in no group. Each file's first copy is used.
    std::vector<ArchiveFile> files(4);
    files[0].data = Bytes("zero");
    files[1].data = Bytes("one");
    files[1].wave_data = Bytes("waves");
    files[2].data = Bytes("two");
    files[3].data = Bytes("three");

    const SoundArchive a = SoundArchive::Load(Rsar(files, {.groups = {{0, 1}, {1, 2}}}));

    REVOSF2_CHECK(Text(a.FileData(0)) == "zero" && Text(a.FileData(1)) == "one" && Text(a.FileData(2)) == "two");
    REVOSF2_CHECK(Text(a.WaveData(1)) == "waves" && a.WaveData(0).empty());
    REVOSF2_CHECK(a.FileData(3).empty() && !a.FileMissing(3) && !a.Truncated());
}

void TestDamagedNameTableLeavesSoundsNumbered()
{
    // Point the one name of an archive past its end. The archive still loads, and its sound goes by number.
    const std::vector<ArchiveFile> files = {
        {Rseq({0x3c, 0x64, 0x18, 0xff})},
        {Rbnk({TestInstrument{}}), Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)})}};
    std::vector<uint8_t> bytes = Rsar(files, {.sequences = {{.file_id = 0}}, .banks = {1}, .names = {"SEQ_A"}});
    auto u32 = [&](std::size_t at)
    {
        return static_cast<std::size_t>(bytes[at] << 24 | bytes[at + 1] << 16 | bytes[at + 2] << 8 | bytes[at + 3]);
    };
    const std::size_t symb_base = u32(0x10) + 8;
    const std::size_t name_offset = symb_base + u32(symb_base) + 4;
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(name_offset), 4, uint8_t{0xff});

    const SoundArchive archive = SoundArchive::Load(bytes);
    const Performance p = PerformSound(archive, 0);

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    REVOSF2_CHECK(archive.NamesDamaged() && archive.Sounds()[0].name == "SEQ_0" && note_ons == 1);
}

void TestTruncatedArchiveKeepsWhatItHolds()
{
    // Cut the archive in the second group's wave data: the first group's files stay whole, and so do the second group's
    // files, but not their wave data.
    std::vector<ArchiveFile> files(2);
    files[0].data = Bytes("first");
    files[0].wave_data = Bytes("first waves");
    files[1].data = Bytes("second");
    files[1].wave_data = Bytes("second waves");
    const std::vector<uint8_t> bytes = Rsar(files, {.groups = {{0}, {1}}});

    const SoundArchive a = SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end() - 4));

    REVOSF2_CHECK(a.Truncated());
    REVOSF2_CHECK(Text(a.FileData(0)) == "first" && Text(a.WaveData(0)) == "first waves");
    REVOSF2_CHECK(Text(a.FileData(1)) == "second" && a.WaveData(1).empty() && a.WaveDataMissing(1));
    REVOSF2_CHECK(!a.FileMissing(1) && !a.WaveDataMissing(0));
}

void TestCopyWithWaveDataIsPreferred()
{
    // Group 0 holds the file whole but its wave data past the end; group 1 holds both. Use group 1's copy.
    std::vector<ArchiveFile> files(1);
    files[0].data = Bytes("bank");
    files[0].wave_data = Bytes("waves");
    std::vector<uint8_t> bytes = Rsar(files, {.groups = {{0}, {0}}});
    const SoundArchive whole = SoundArchive::Load(bytes);

    REVOSF2_CHECK(Text(whole.FileData(0)) == "bank" && Text(whole.WaveData(0)) == "waves");
}

void TestTruncatedArchiveWhoseTablesCantBeReadIsReported()
{
    // Cut the archive within the INFO block's tables.
    std::vector<ArchiveFile> files(1);
    files[0].data = Bytes("data");
    const std::vector<uint8_t> bytes = Rsar(files);
    const std::size_t info = (bytes[0x18] << 24) | (bytes[0x19] << 16) | (bytes[0x1a] << 8) | bytes[0x1b];
    const std::size_t cut = info + 0x20;

    std::string message;
    try
    {
        SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(cut)));
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    REVOSF2_CHECK(message.starts_with("truncated archive: " + std::to_string(cut) + " of " +
                                      std::to_string(bytes.size()) + " bytes; cannot read tables (read out of bounds"));
}

void TestOtherPlatformsArchivesAreReported()
{
    auto message_for = [](const char* magic)
    {
        std::vector<uint8_t> bytes(0x40, 0);
        std::memcpy(bytes.data(), magic, 4);
        try
        {
            SoundArchive::Load(bytes);
        }
        catch (const FormatError& e)
        {
            return std::string(e.what());
        }

        return std::string();
    };

    REVOSF2_CHECK(message_for("CSAR") == "unsupported CSAR archive (3DS); expected RSAR (Wii)");
    REVOSF2_CHECK(message_for("FSAR") == "unsupported FSAR archive (Wii U/Switch); expected RSAR (Wii)");
    REVOSF2_CHECK(message_for("SDAT") == "unsupported SDAT archive (DS); expected RSAR (Wii)");
    REVOSF2_CHECK(message_for("RIFF") == "bad magic, expected RSAR");
}

void TestNamesHaveNoControlCharacters()
{
    // Replace unsafe name characters with '?': newline, terminal escape, DEL, C1 NEL, Unicode line/paragraph separators
    // and invalid UTF-8 (lone byte, overlong encoding, surrogate and truncated character). Preserve valid é and emoji.
    const std::vector<std::pair<std::string, std::string>> kPieces = {
        {"Seq", "Seq"},           {"\n", "?"},
        {"\x1b[2J", "?[2J"},      {"\x7f", "?"},
        {"\xc2\x85", "?"},        {"\xe2\x80\xa8", "?"},
        {"\xe2\x80\xa9", "?"},    {"\xff", "?"},
        {"\xc0\xaf", "??"},       {"\xed\xa0\x80", "???"},
        {"\xc3\xa9", "\xc3\xa9"}, {"\xf0\x9f\x98\x80", "\xf0\x9f\x98\x80"},
        {"\xe2\x80", "??"}};
    std::string name, printable;
    for (const auto& [piece, shown] : kPieces)
    {
        name += piece;
        printable += shown;
    }

    std::vector<ArchiveFile> files(1);
    files[0].data = Rseq({0xff});

    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = {{}, {}}, .names = {name}}));

    REVOSF2_CHECK(archive.Sounds().size() == 2 && archive.Sounds()[0].name == printable);
    REVOSF2_CHECK(archive.Sounds()[1].name == "SEQ_1");
}

void TestBrokenBankReferencesAreReported()
{
    // Sound 0 names bank 5. No such bank exists. Sound 1 names bank 0. No group holds bank 0's file. Sound 2 names bank
    // 1. Bank 1's region plays wave 3 of an RWAR with one wave.
    std::vector<ArchiveFile> files(3);
    files[0].data = Rseq({0xff});
    files[1].data = Rbnk({TestInstrument{}});
    files[2].data = Rbnk({TestInstrument{{{.wave_index = 3}}}});
    files[2].wave_data = Rwar({Rwav(4, {1, 2, 3, 4})});
    const SoundArchive archive = SoundArchive::Load(
        Rsar(files, {.sequences = {{0, 0, 5}, {0, 0, 0}, {0, 0, 1}}, .banks = {1, 2}, .groups = {{0, 2}}}));
    BankSet none(archive, archive.Sounds()[0]);
    BankSet unstored(archive, archive.Sounds()[1]);
    BankSet missing_wave(archive, archive.Sounds()[2]);

    const Pcm* wave = missing_wave.GetWave(3);
    missing_wave.GetWave(3); // repeat the lookup without reporting the error again

    REVOSF2_CHECK(none.Errors() == std::vector<std::string>{"bank 5 doesn't exist (the archive has 2)"});
    REVOSF2_CHECK(unstored.Errors() == std::vector<std::string>{"bank BANK_0 is not stored in the archive"});
    REVOSF2_CHECK(!wave && missing_wave.Errors() ==
                               std::vector<std::string>{"bank BANK_1's wave archive has no wave 3 (it has 1)"});
}

void TestWaveDataPastTheEndIsReported()
{
    // The bank's file is whole, but its wave data lies past the end of the truncated archive.
    std::vector<ArchiveFile> files = LoopingWaveFiles({0x3c, 0x64, 0x30, 0xff});
    const std::vector<uint8_t> bytes = Rsar(files, {.sequences = {{0, 0, 0}}, .banks = {1}});
    const SoundArchive archive = SoundArchive::Load(std::vector<uint8_t>(bytes.begin(), bytes.end() - 8));
    BankSet banks(archive, archive.Sounds()[0]);

    const Performance p = Perform(archive, 0, banks, {});

    REVOSF2_CHECK(archive.Truncated() && NoteCount(p) == 0);
    REVOSF2_CHECK(banks.Errors() == std::vector<std::string>{"bank BANK_0's waves extend past the truncated archive"});
}

void TestSequenceStartingPastItsEndPlaysNothing()
{
    // Start track 0 at 2^32 - 2, beyond the bytecode. The track finishes immediately.
    std::vector<ArchiveFile> files(1);
    files[0].data = Rseq({0x3c, 0x64, 0x30, 0xff});
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = {{0, 0xfffffffe}}}));

    const Performance p = PerformSound(archive, 0);

    REVOSF2_CHECK(p.end_tick == 0 && p.presets.empty() && p.warnings.empty());
}

void TestOnlyAJumpBackToPlayedCommandsIsALoop()
{
    // The first sequence skips a FIN, waits a tick and jumps back to the FIN it skipped. The second plays key 60 for a
    // tick and jumps back to its start, a loop it repeats until the performance reaches 10 s.
    const SoundArchive archive = SequenceArchive({{0x89, 0x00, 0x00, 0x05, 0xff, 0x80, 0x01, 0x89, 0x00, 0x00, 0x04},
                                                  {0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x00}});

    const Performance once = PerformSound(archive, 0);
    const Performance looping = PerformSound(archive, 1);

    REVOSF2_CHECK(!once.loop && once.loops == 0);
    REVOSF2_CHECK(looping.loop && looping.loop->start == 0 && looping.loop->end == 1);
    REVOSF2_CHECK(looping.seconds >= 10.0 && looping.seconds < 10.1);
}

void TestMainLoopWaitsForAFade()
{
    // Track 0 opens track 1, fades from volume 127 to 64 over 2000 ticks and then holds a looping note. The fade uses a
    // time prefix, a command of the builds from 2008. Track 1 loops every 48 ticks. The main loop starts once the fade
    // has ended.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0e, 0xa3, 0xc1, 0x40, 0x07, 0xd0,
                                           0x3c, 0x7f, 0x00, 0xff, 0x80, 0x30, 0x89, 0x00, 0x00, 0x0e},
                                          {.track_flags = 3});

    REVOSF2_CHECK(p.loop && p.loop->start >= 2000 && p.loop->end - p.loop->start == 48);
}

void TestLevelTooLoudForMidiIsCut()
{
    // Volume 64 and volume2 255 play a note at full level. The voice holds it there. With CC7 at 64, that level
    // requires CC11 252: more than MIDI has. Every CC11 is scaled down to fit, and the SFZ files add the cut back.
    const Performance p = PerformCommands({0xc1, 0x40, 0xd5, 0xff, 0x3c, 0x7f, 0x30, 0xff});

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 64}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}};
    REVOSF2_CHECK(Controls(p, EventKind::kVolume) == kVolume && Controls(p, EventKind::kExpression) == kExpression);
    REVOSF2_CHECK(std::fabs(p.level_cut_db - 40.0 * std::log10(127.0 / 64.0)) < 1e-9);
}

void TestFullLevelStopsTheTrackVolumes()
{
    // Volume 255 would play a note at four times full level, but the voice stops it at full level. The fade from 255 to
    // 128 that follows makes no difference and requires no level cut.
    const Performance p = PerformCommands({0xc1, 0xff, 0x3c, 0x7f, 0x18, 0xc1, 0x80, 0x3c, 0x7f, 0x18, 0xff});

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 127}};
    REVOSF2_CHECK(Controls(p, EventKind::kVolume) == kVolume && Controls(p, EventKind::kExpression) == kExpression);
    REVOSF2_CHECK(p.level_cut_db == 0.0);
}

void TestLegatoLeavesOutTheRegionsVolume()
{
    // With volume2 64 and tie on, play key 60 at velocity 100 on a region of volume 64, then key 62 at 100 and key 64
    // at 127. NW4R SND continues the channel with the velocity's gain alone and leaves the region's volume out. The
    // MIDI note starts at 127, the highest velocity the legatos reach. CC11 takes it down to 100. The note continues
    // through both legatos. There, CC11 also makes up for the zone's region volume, by 127 / 64 in power.
    const Performance p =
        PerformCommands({0xd5, 0x40, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0x40, 0x7f, 0x30, 0xff},
                        {.region = {.volume = 64}});

    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 50}, {48, 71}, {96, 90}};
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    REVOSF2_CHECK(Controls(p, EventKind::kExpression) == kExpression && NoteOns(p) == kNotes);
    REVOSF2_CHECK(p.level_cut_db == 0.0);
}

void TestRisingLegatoCarriesItsNoteOn()
{
    // With tie on and note wait off, play key 60 at velocity 40, then key 62 at 100 at tick 10 and key 60 at 70 at tick
    // 20, and finish at tick 30. NW4R SND plays one channel throughout, and its sample runs on. So does the MIDI note.
    // The note starts at velocity 100, the highest the legatos reach. Until then, CC11 takes it down to 40.
    const Performance p = PerformCommands({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3e,
                                           0x64, 0x00, 0x80, 0x0a, 0x3c, 0x46, 0x00, 0x80, 0x0a, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}, {20, 89}};
    REVOSF2_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kExpression) == kExpression);
    REVOSF2_CHECK(p.level_cut_db == 0.0 && !p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestRisingLegatoBesideATailTakesAnotherPart()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then turn tie on. That detaches 60 while its release
    // sounds. Play key 62 at velocity 40 and raise it to 100 at tick 10. The track goes on to another part. There 62
    // starts at 100 with CC11 taking it down to 40, and 60's release keeps the first part's CC11.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc8, 0x01,
                                           0x3e, 0x28, 0x00, 0x80, 0x05, 0x3e, 0x64, 0x00, 0x80, 0x05, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {5, 62, 100}};
    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSecondPart = {{5, 51}, {10, 127}};
    const std::vector<int> kParts = {0, 1};
    REVOSF2_CHECK(NoteOns(p) == kNotes && NoteParts(p) == kParts);
    REVOSF2_CHECK(PartControls(p, EventKind::kExpression, 0) == kFirstPart &&
                  PartControls(p, EventKind::kExpression, 1) == kSecondPart);
    REVOSF2_CHECK(!p.approximations.contains("legato (tie or mono) played as a new note"));
}

void TestDetachedTailKeepsItsSettings()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Set the volume to 64 at tick 2. In the game the release keeps its volume. The track
    // therefore goes on to another part, and that part gets the new volume. The first part keeps 127.
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01, 0xc1, 0x40, 0x80, 0x30, 0xff});

    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}};
    const std::vector<std::pair<uint32_t, int>> kSecondPart = {{1, 127}, {2, 64}};
    REVOSF2_CHECK(PartControls(p, EventKind::kVolume, 0) == kFirstPart &&
                  PartControls(p, EventKind::kVolume, 1) == kSecondPart);
    REVOSF2_CHECK(!p.approximations.contains("detached notes (still receive track controller changes in MIDI)"));
}

void TestSweepLeavesTheOlderNoteItsPitch()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then key 64 for 10 with a sweep from 4 semitones up
    // (sweep pitch 256, in 64ths). 60's release still sounds. 64 therefore goes on another part, and its sweep bends
    // that part alone.
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xe3, 0x01, 0x00, 0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff});

    bool first_part_bends = false;
    float second_part_start = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == 0)
        {
            first_part_bends = true;
        }
        if (e.kind == EventKind::kPitch && e.part == 1 && e.tick == 5)
        {
            second_part_start = e.semitones;
        }
    }
    const std::vector<int> kParts = {0, 1};
    REVOSF2_CHECK(NoteParts(p) == kParts && !first_part_bends && second_part_start == 4.0f);
}

void TestPartLeftTakesTheBendInItsTick()
{
    // With note wait off and release 20, play key 60 for 5 ticks, then bend the track a semitone up and play key 64
    // with a sweep from 4 semitones up. 60's release still sounds. 64 therefore goes on another part. The track still
    // holds 60, and that note's channel takes the bend in the same frame as 64's channel. So the first part bends at
    // tick 5 too, without the sweep.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x05, 0x80, 0x05, 0xc4,
                                           0x40, 0xe3, 0x01, 0x00, 0x40, 0x64, 0x0a, 0x80, 0x0a, 0xff});

    std::vector<std::pair<uint32_t, float>> first_part;
    float second_part_start = 0.0f;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPitch && e.part == 0)
        {
            first_part.emplace_back(e.tick, e.semitones);
        }
        if (e.kind == EventKind::kPitch && e.part == 1 && e.tick == 5)
        {
            second_part_start = e.semitones;
        }
    }

    const std::vector<std::pair<uint32_t, float>> kFirstPart = {{5, 1.0f}};
    const std::vector<int> kParts = {0, 1};
    REVOSF2_CHECK(NoteParts(p) == kParts && first_part == kFirstPart && second_part_start == 5.0f);
}

// The note-ons of `p`'s track 0, and whether a legato was played as a new note.
std::pair<std::ptrdiff_t, bool> NoteOnsAndRestarts(const Performance& p)
{
    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    return {note_ons, p.approximations.contains("legato (tie or mono) played as a new note")};
}

void TestLegatoAfterAnEnvelopeResetCarriesOn()
{
    // With tie on and attack 100, play key 60, reset the envelope settings and tie key 62 on. The reset leaves the
    // channel's attack as it was. The MIDI note therefore continues.
    const Performance p = PerformCommands({0xc8, 0x01, 0xd0, 0x64, 0x3c, 0x64, 0x18, 0xfb, 0x3e, 0x64, 0x18, 0xff});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    REVOSF2_CHECK(note_ons == 1 && !restarted);
}

void TestLegatoOnADecayedNoteStaysSilent()
{
    // With tie on, play key 60 on a region that ignores note-off and decays to silence at once. Tie key 62 on 48 ticks
    // later. The channel has decayed by then, and a legato doesn't start the envelope again. No MIDI note starts.
    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x30, 0xff},
                                          {.region = {.sustain = 0, .note_off_type = 1}});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    REVOSF2_CHECK(note_ons == 1 && !restarted);
}

void TestLegatoBelowHearingKeepsItsMidiNote()
{
    // With tie on, sustain 0, decay 100 and hold 0, play key 60 with note wait off. After 100 ticks the channel has
    // sunk below hearing (kInaudibleSustain) but hasn't faded to kFadedAttenuation. Its MIDI note still sounds. Then
    // tie key 62 on, once after setting release 64 and once at velocity 127. The game's legato doesn't start the
    // envelope again. Neither legato starts a new MIDI note, and velocity 127 doesn't start the note higher on a later
    // pass.
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    auto at_ms = [&](int attenuation)
    {
        return static_cast<double>(FramesToLevel(kEnvelope, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };

    const Performance released =
        PerformCommands({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64,
                         0x01, 0x80, 0x64, 0xd3, 0x40, 0x3e, 0x64, 0x01, 0x80, 0x30, 0xff});
    const Performance louder = PerformCommands({0xc8, 0x01, 0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c,
                                                0x64, 0x01, 0x80, 0x64, 0x3e, 0x7f, 0x01, 0x80, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kStruck = {{0, 60, 100}};
    REVOSF2_CHECK(at_ms(kInaudibleSustain) < 100 * kTickMs && 100 * kTickMs < at_ms(kFadedAttenuation));
    REVOSF2_CHECK(NoteOns(released).size() == 1 &&
                  !released.approximations.contains("legato (tie or mono) played as a new note"));
    REVOSF2_CHECK(NoteOns(louder) == kStruck);
}

void TestMidiNoteOfAnEndlessNoteEndsBelowHearing()
{
    // With sustain 0, decay 100 and hold 0, play key 60 with note wait off and no length. Then wait 192 ticks (2 s) and
    // end. With no length, only the track's end releases the note. The MIDI note and the performance end where the
    // channel has faded to kFadedAttenuation. With sustain 5, the level stays above that, and the MIDI note and the
    // performance end below hearing (kInaudibleSustain). The same note with a length of 96 ticks keeps its MIDI note
    // until that length runs out.
    const Performance faded =
        PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff});
    const Performance shallow =
        PerformCommands({0xd2, 0x05, 0xd1, 0x64, 0xb1, 0x00, 0xc7, 0x00, 0x3c, 0x64, 0x00, 0x80, 0x81, 0x40, 0xff});
    const Performance timed = PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0x60, 0xff});

    auto at_ms = [](int attenuation, int sustain)
    {
        const EnvelopeValues v{127, 100, sustain, 0, 127};
        return static_cast<double>(FramesToLevel(v, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };
    auto off_tick = [](const Performance& p)
    {
        const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
        return off == p.tracks[0].end() ? 0u : off->tick;
    };
    auto ends_at = [&](const Performance& p, double ms)
    {
        const double off_ms = off_tick(p) * kTickMs;
        return NoteCount(p) == 1 && off_ms >= ms && off_ms < ms + kTickMs + kFrameMs && p.seconds * 1000.0 >= ms &&
               p.seconds * 1000.0 < ms + kTickMs + kFrameMs;
    };
    const double faded_ms = at_ms(kFadedAttenuation, 0), quiet_ms = at_ms(kInaudibleSustain, 5);
    REVOSF2_CHECK(quiet_ms < faded_ms && faded_ms < 1900.0);
    REVOSF2_CHECK(ends_at(faded, faded_ms));
    REVOSF2_CHECK(ends_at(shallow, quiet_ms));
    REVOSF2_CHECK(off_tick(timed) == 96);
}

void TestHeldNoteBelowHearingEndsTheSequence()
{
    // With sustain 0, decay 100 and hold 0, play key 60 for 10,000 ticks and end. The track waits out a held note. The
    // note sinks below hearing long before its length would release it, and no track reads on. The sequence ends where
    // the note has faded to kFadedAttenuation. A tie's MIDI note ends there too. The sequence doesn't hold the sound
    // for 10 seconds. With sustain 5, the level stays above that, and the sequence ends below hearing
    // (kInaudibleSustain).
    const Performance faded = PerformCommands({0xd2, 0x00, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0xce, 0x10, 0xff});
    const Performance shallow = PerformCommands({0xd2, 0x05, 0xd1, 0x64, 0xb1, 0x00, 0x3c, 0x64, 0xce, 0x10, 0xff});

    auto at_ms = [](int attenuation, int sustain)
    {
        const EnvelopeValues v{127, 100, sustain, 0, 127};
        return static_cast<double>(FramesToLevel(v, -static_cast<float>(attenuation)) + 1) * kFrameMs;
    };
    auto ends_at = [](const Performance& p, double ms)
    {
        const auto off = std::ranges::find(p.tracks[0], EventKind::kNoteOff, &TrackEvent::kind);
        const double off_ms = off == p.tracks[0].end() ? 0.0 : off->tick * kTickMs;
        return !p.holds && !p.truncated && !p.loop && NoteCount(p) == 1 && off_ms >= ms &&
               off_ms < ms + kTickMs + kFrameMs && p.seconds * 1000.0 >= ms &&
               p.seconds * 1000.0 < ms + kTickMs + kFrameMs;
    };
    REVOSF2_CHECK(ends_at(faded, at_ms(kFadedAttenuation, 0)));
    REVOSF2_CHECK(ends_at(shallow, at_ms(kInaudibleSustain, 5)));
}

void TestSilentLegatoIsNotReported()
{
    // With tie on, play key 60 at velocity 0, then tie key 62 on at velocity 0. Neither sounds, and the legato isn't
    // reported as a new note.
    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x00, 0x18, 0x3e, 0x00, 0x18, 0xff});

    const auto [note_ons, restarted] = NoteOnsAndRestarts(p);
    REVOSF2_CHECK(note_ons == 0 && !restarted);
}

void TestSilentNoteLeavesTheTailItsPitch()
{
    // With note wait off and release 64, play key 60 for 48 ticks and turn tie on a tick later. That detaches the note
    // while its release sounds, and the track goes on to another part. Then play key 60 at velocity 0 with a sweep down
    // from 18 semitones up. The game plays that note silently, and the tail keeps its pitch. No part's pitch moves.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xc8,
                                           0x01, 0xe3, 0x04, 0x80, 0x3c, 0x00, 0x30, 0x80, 0x30, 0xff});

    const auto moved = [](const TrackEvent& e)
    {
        return e.kind == EventKind::kPitch && e.semitones != 0.0f;
    };
    REVOSF2_CHECK(NoteCount(p) == 1 && std::ranges::none_of(p.tracks[0], moved));
}

void TestTieGivesTheTailTheTicksVolume()
{
    // With note wait off and release 20, hold key 60, and 5 ticks later set the track's volume to 64 and turn tie on.
    // Tie releases the note and detaches it, but the game first gives its channel the track's settings, the volume
    // among them. So the note's part takes CC7 64 at tick 5, before the track moves to another part.
    const Performance p = PerformCommands(
        {0xc7, 0x00, 0xd3, 0x14, 0x3c, 0x64, 0x00, 0x80, 0x05, 0xc1, 0x40, 0xc8, 0x01, 0x80, 0x30, 0xff});

    std::vector<std::pair<uint32_t, int>> first_part;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kVolume && e.part == 0)
        {
            first_part.emplace_back(e.tick, e.value);
        }
    }

    const std::vector<std::pair<uint32_t, int>> kFirstPart = {{0, 127}, {5, 64}};
    REVOSF2_CHECK(first_part == kFirstPart);
}

void TestNoteAboveVelocity127TakesItsLevel()
{
    // Velocity range 255 takes velocity 100 to 200. The MIDI note plays at 127, and CC11 the rest of the way. That
    // takes the sequence down by 40 log10(200 / 127) dB.
    const Performance p = PerformCommands({0xb3, 0xff, 0x3c, 0x64, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 127}};
    REVOSF2_CHECK(NoteOns(p) == kNotes && std::fabs(p.level_cut_db - 40.0 * std::log10(200.0 / 127.0)) < 1e-9);
    REVOSF2_CHECK(!p.approximations.contains("velocity above 127 (played at 127)"));
}

void TestMuteStopLeavesDetachedTails()
{
    // With note wait off and release 64, play key 60 for 96 ticks and turn tie on at tick 1. That detaches the note
    // while its release sounds. Then play key 62. Mute mode 3 at tick 2 stops 62 at once, but not 60's release: the
    // track detached 60. CC120 goes to 62's part alone.
    const Performance p = PerformCommands({0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01,
                                           0x3e, 0x64, 0x00, 0x80, 0x01, 0xdd, 0x03, 0x80, 0x30, 0xff});

    std::vector<std::pair<uint32_t, int>> sound_offs; // tick, part
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kSoundOff)
        {
            sound_offs.emplace_back(e.tick, e.part);
        }
    }
    const std::vector<int> kParts = {0, 1};
    const std::vector<std::pair<uint32_t, int>> kSoundOffs = {{2, 1}};
    REVOSF2_CHECK(NoteParts(p) == kParts && sound_offs == kSoundOffs);
}

void TestRisingLegatoKeepsToItsRegion()
{
    // The regions split at velocity 63. With note wait off, tie key 60 at velocity 40, and raise it to 100 at tick 10.
    // The MIDI note can start no higher than 63. Above that, the synth would select the other region's zone. CC11
    // therefore has to rise above 127 for the legato, and the whole sequence comes down by 40 log10(100 / 63) dB.
    const Performance p =
        PerformCommands({0xc8, 0x01, 0xc7, 0x00, 0x3c, 0x28, 0x00, 0x80, 0x0a, 0x3c, 0x64, 0x00, 0x80, 0x0a, 0xff},
                        {.velocity_regions = {TestRegion{}, TestRegion{.pan = 32}}, .upper_velocities = {63, 127}});

    const double kCutDb = 40.0 * std::log10(100.0 / 63.0);
    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 63}};
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 51}, {10, 127}};
    REVOSF2_CHECK(NoteOns(p) == kNotes && Controls(p, EventKind::kExpression) == kExpression);
    REVOSF2_CHECK(std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestLevelCutFitsTheRunThatsKept()
{
    // Tie key 60 at velocity 127 on a region of volume 32. At tick 24, change the filter and play key 62 at velocity
    // 100. The first pass plays key 62 as a new preset and note, with CC11 253 to undo the region's attenuation (127 /
    // 32 in power). With the filter on a controller, the second pass continues the MIDI note at 100 / 127 of its
    // velocity. That takes CC11 to 199. The level cut has to follow that peak.
    const Performance p = PerformCommands(
        {0xc8, 0x01, 0x3c, 0x7f, 0x18, 0xd8, 0x20, 0x3e, 0x64, 0x18, 0x80, 0x30, 0xff}, {.region = {.volume = 32}});

    const double kCutDb = 40.0 * std::log10(100.0 * std::sqrt(127.0 / 32.0) / 127.0);
    const std::vector<std::pair<uint32_t, int>> kExpression = {{0, 81}, {24, 127}};
    REVOSF2_CHECK(Controls(p, EventKind::kExpression) == kExpression && std::fabs(p.level_cut_db - kCutDb) < 1e-9);
}

void TestTiedNoteTakesPartInTheMainLoop()
{
    // Track 0 opens track 1, ties key 72 for 30 ticks and then for good. Track 1 loops a 26-tick key 60. The tie at
    // tick 30 changes nothing in MIDI. It still falls in track 1's pass from tick 26. So the main loop is the pass
    // after that.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0d, 0xc8, 0x01, 0x48, 0x78, 0x1e,
                                           0x48, 0x78, 0x00, 0x3c, 0x64, 0x1a, 0x89, 0x00, 0x00, 0x0d},
                                          {.track_flags = 3});

    REVOSF2_CHECK(p.loop && p.loop->start == 52 && p.loop->end == 78);
}

void TestSilentLegatoCarriesItsNoteOn()
{
    // With monophonic on (a 2008 command) and note wait off, play key 60 for 30 ticks. At tick 10, change the release.
    // That makes another preset. At tick 20, play key 60 at velocity 0, then finish at tick 50. The MIDI note goes on,
    // silenced by CC11. A note-off would leave a release tail. The preset that isn't used isn't made.
    const Performance p = PerformCommands({0xb2, 0x01, 0xc7, 0x00, 0x3c, 0x64, 0x1e, 0x80, 0x0a, 0xd3, 0x40, 0x80, 0x0a,
                                           0x3c, 0x00, 0x3c, 0x80, 0x1e, 0xff});

    std::vector<std::tuple<uint32_t, EventKind, int>> events; // tick, kind and CC11 value
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kNoteOn || e.kind == EventKind::kNoteOff || e.kind == EventKind::kExpression)
        {
            events.emplace_back(e.tick, e.kind, e.kind == EventKind::kExpression ? e.value : 0);
        }
    }

    const std::vector<std::tuple<uint32_t, EventKind, int>> kEvents = {{0, EventKind::kExpression, 127},
                                                                       {0, EventKind::kNoteOn, 0},
                                                                       {20, EventKind::kExpression, 0},
                                                                       {50, EventKind::kNoteOff, 0}};
    REVOSF2_CHECK(events == kEvents && p.presets.size() == 1);
}

void TestLegatosChangeOfKeyIsNoChangeOfTheTracks()
{
    // Track 0 opens track 1, sets tempo 1023, turns on tie and portamento (time 100) and holds key 60. At tick 1, track
    // 0 changes the low-pass during the note and plays key 62 legato. The first pass plays that as a new MIDI note. The
    // second, with the filter on a controller, continues the note with a bend of 2 semitones. Track 1 loops every 6
    // ticks. Only the first pass bends back to 0 in MIDI. On both passes, though, the sweep's end counts as a change on
    // track 0. Both passes then select the same main loop.
    const Performance p = PerformCommands(
        {0x88, 0x01, 0x00, 0x00, 0x1d, 0xe1, 0x03, 0xff, 0xc8, 0x01, 0xce, 0x01, 0xcf, 0x64, 0xc7, 0x00, 0x3c, 0x64,
         0x00, 0x80, 0x01, 0xd8, 0x10, 0xc7, 0x01, 0x3e, 0x64, 0x00, 0xff, 0x80, 0x06, 0x89, 0x00, 0x00, 0x1d},
        {.track_flags = 3});

    REVOSF2_CHECK(p.loop && p.loop->start == 2562 && p.loop->end == 2568);
}

void TestMonoLegatoTakesItsLengthWithTieOn()
{
    // With monophonic (2008) and tie on, play key 60 for 48 ticks, then key 62 for 24 ticks, and wait. A monophonic
    // track gives its channel the new length even in a tie. The note ends at tick 72.
    const Performance p =
        PerformCommands({0xb2, 0x01, 0xc8, 0x01, 0x3c, 0x64, 0x30, 0x3e, 0x64, 0x18, 0x80, 0x60, 0xff});

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {72, EventKind::kNoteOff}};
    REVOSF2_CHECK(NoteEvents(p) == kNotes);
}

void TestNoteReleasedBeforeItsFirstFrameIsSilent()
{
    // With note wait off, play key 60 and turn monophonic on (2008). That releases the note. Released at once, before
    // the envelope's first update, the note releases from -90.4 dB and makes no sound. Released a tick later, it makes
    // a sound.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0x3c, 0x64, 0x30, 0xb2, 0x01, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb2, 0x01, 0x80, 0x30, 0xff}});

    const Performance at_once = PerformSound(archive, 0);
    const Performance later = PerformSound(archive, 1);

    const std::vector<std::pair<uint32_t, EventKind>> kNotes = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    REVOSF2_CHECK(NoteCount(at_once) == 0 && NoteEvents(later) == kNotes);
}

void TestWhatMidiCantSeparateIsReported()
{
    // In the first sequence, all 16 tracks release a key 60 with a slow release by turning tie on a tick later. Then
    // they change the volume during the release. Every channel goes to a track, and none is left for another part. MIDI
    // then can't keep the new volume away from the releasing notes. The second sequence puts the track's pan left of a
    // region panned right.
    std::vector<uint8_t> all_tracks;
    for (uint8_t t = 1; t < 16; t++)
    {
        all_tracks.insert(all_tracks.end(), {0x88, t, 0x00, 0x00, 15 * 5});
    }
    all_tracks.insert(all_tracks.end(), {0xc7, 0x00, 0xd3, 0x40, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xc8, 0x01, 0x80, 0x01,
                                         0xc1, 0x40, 0x80, 0x30, 0xff});
    const SoundArchive archive = SequenceArchive({all_tracks, {0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff}},
                                                 {.track_flags = 0xffff, .region = {.pan = 100}});

    const Performance detached = PerformSound(archive, 0);
    const Performance panned = PerformSound(archive, 1);

    const std::string kDetached = "detached notes (still receive track controller changes in MIDI)";
    const std::string kPans = "combined region and track pan (using the synth pan law)";
    REVOSF2_CHECK(detached.approximations.contains(kDetached) && !detached.approximations.contains(kPans));
    REVOSF2_CHECK(panned.approximations.contains(kPans) && !panned.approximations.contains(kDetached));
}

void TestOneShotWaveEndsAsTheDspPlaysIt()
{
    // Play a one-shot wave at four times AX's rate, 384 samples a frame, and wait for it. The wave's voice starts the
    // frame after the note's frame. NW4R SND reads a wave's end one frame after the frame that passes it, at the start
    // of the frame after that. A wave of 3072 samples passes its end in frame 8. The channel has then gone at 30 ms, in
    // time for tick 3 (31.25 ms). A sample more takes the end to frame 9, after tick 3. The track then waits until tick
    // 4.
    const std::vector<uint8_t> kCommands = {0x3c, 0x7f, 0x00, 0xff};
    std::vector<uint32_t> note_offs, ends;
    for (const uint32_t length : {3072u, 3073u})
    {
        const std::vector<int16_t> samples(length, 1000);
        const Performance p = PerformCommands(kCommands, {.wave = Rwav(length, samples, 1, false, 4 * 32000)});
        for (const auto& [tick, kind] : NoteEvents(p))
        {
            if (kind == EventKind::kNoteOff)
            {
                note_offs.push_back(tick);
            }
        }
        ends.push_back(p.end_tick);
    }

    REVOSF2_CHECK(note_offs == (std::vector<uint32_t>{3, 4}) && ends == (std::vector<uint32_t>{3, 4}));
}

void TestTicksFallInTheGamesSoundFrames()
{
    // At tempo 40, a tick lasts 31.25 ms. Tick 12 then falls on the start of frame 125, at 375 ms. NW4R SND's single
    // precision falls a little short and puts the tick at the end of frame 124. MIDI events of tick 12 go at the start
    // of that frame: 372 ms, or tick 11.904.
    const Performance p = PerformCommands({0xe1, 0x00, 0x28, 0x80, 0x0c, 0xff});

    REVOSF2_CHECK(p.frame_ticks.size() == 13 && std::fabs(p.frame_ticks[12] - 372.0 / 31.25) < 1e-4);
}

void TestOddArgumentsAreReadAsTheGameReadsThem()
{
    // A volume ramp over 0x8000 ticks: a negative count in NW4R SND's 16 bits, applied at once. A wait of 128 ticks in
    // seven bytes (reading only five would leave bytes to be read as a program change). And timebase 0. That stops the
    // sequence's timing.
    const SoundArchive archive =
        SequenceArchive({{0xa3, 0xc1, 0x40, 0x80, 0x00, 0x3c, 0x64, 0x30, 0xff},
                         {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x81, 0x00, 0x3c, 0x64, 0x30, 0xff},
                         {0xb0, 0x00, 0x80, 0x30, 0xff}});

    const Performance ramp = PerformSound(archive, 0);
    const Performance wait = PerformSound(archive, 1);
    const Performance timebase = PerformSound(archive, 2);

    const std::vector<std::pair<uint32_t, int>> kVolume = {{0, 64}};
    const std::vector<std::tuple<uint32_t, int, int>> kWaited = {{128, 60, 100}};
    const std::vector<std::string> kStopped = {"timebase 0: the sequence stops advancing"};
    REVOSF2_CHECK(Controls(ramp, EventKind::kVolume) == kVolume && NoteOns(wait) == kWaited);
    REVOSF2_CHECK(timebase.warnings == kStopped && timebase.end_tick == 0 && timebase.timebase == 48);
}

void TestSequenceThatStopsAdvancingPlaysOut()
{
    // The first three sequences turn note wait off, play key 60, wait and set tempo 0. A note of 96 ticks then holds
    // for good, and plays for 10 s at MIDI's 120 BPM. A note of one tick at release 90 has already been released, and
    // ends as it would have; the third sequence also finishes on the tick that stops. A fourth sequence plays the same
    // notes without tempo 0, and both end with it.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xe1, 0x00, 0x00, 0xff},
                         {0xc7, 0x00, 0xd3, 0x5a, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xff}});

    const Performance held = PerformSound(archive, 0);
    const Performance released = PerformSound(archive, 1);
    const Performance ending = PerformSound(archive, 2);
    const Performance unstopped = PerformSound(archive, 3);

    const std::vector<std::string> kStopped = {"tempo 0: the sequence stops advancing"};
    REVOSF2_CHECK(held.warnings == kStopped && held.holds && held.end_tick == 961 && held.tracks[0].back().tick == 961);
    REVOSF2_CHECK(released.warnings == kStopped && !released.holds && unstopped.end_tick > 200 &&
                  released.end_tick == unstopped.end_tick && ending.end_tick == unstopped.end_tick);
}

void TestTieLoopThatHoldsEndsAsAHeldSound()
{
    // The first sequence turns tie on, plays key 60 for 2400 ticks (25 s) and jumps back to the note command. With tie
    // on, that command continues the note. The sound holds for good. The file ends 10 s after the note starts, without
    // loop markers. The second sequence does the same without tie. Each pass then releases the note and strikes it
    // again: a loop of 25 s.
    const SoundArchive archive = SequenceArchive({{0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02},
                                                  {0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x00}});

    const Performance tied = PerformSound(archive, 0);
    const Performance struck = PerformSound(archive, 1);

    REVOSF2_CHECK(tied.holds && !tied.loop && std::fabs(tied.seconds - 10.0) < 0.02);
    REVOSF2_CHECK(!struck.holds && struck.loop && struck.loop->start == 0 && struck.loop->end == 2400);
}

void TestTieLoopOverAOneShotWaveStaysALoop()
{
    // The tied note of the test above, on a one-shot wave of 30 s. The wave is still playing when the pass of 25 s
    // ends. It ends by itself, though. The pass doesn't hold it for good.
    const std::vector<int16_t> kSamples(30 * 32000, 1000);

    const Performance p = PerformCommands({0xc8, 0x01, 0x3c, 0x64, 0x92, 0x60, 0x89, 0x00, 0x00, 0x02},
                                          {.wave = Rwav(30 * 32000, kSamples)});

    REVOSF2_CHECK(!p.holds && p.loop && p.loop->end == 2400);
}

void TestClosingTickReachesTheReleaseTail()
{
    // Release 100 and key 60 for one tick, then volume 0 and FIN in the next. Like the end of a tick, closing the track
    // gives the released note the track's settings. The tail is then silent in the game. MIDI requires CC7 0 for that.
    const Performance p = PerformCommands({0xd3, 0x64, 0x3c, 0x64, 0x01, 0xc1, 0x00, 0xff});

    const std::vector<std::pair<uint32_t, int>> volumes = Controls(p, EventKind::kVolume);
    REVOSF2_CHECK(std::ranges::find(volumes, std::pair<uint32_t, int>{1, 0}) != volumes.end());
}

void TestSilenceAtTheTimeLimitEndsWithTheLastSound()
{
    // Key 60 plays for 48 ticks (0.5 s), then the track waits 100,000 ticks (17 minutes), past the 15-minute limit. The
    // track has been silent since the note. The performance therefore treats the sequence as finished and ends with the
    // note.
    const Performance p = PerformCommands({0x3c, 0x64, 0x30, 0x80, 0x86, 0x8d, 0x20, 0xff});

    REVOSF2_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestOneShotWavePlaysOutAtItsPitch()
{
    // Bend key 60 up by 127/128 of a 12-semitone range. The bend plays a one-shot wave of 4 s in about 2 s. With note
    // wait off, waiting for the wave and setting tempo 0 at once end the wave at the same time.
    const std::vector<int16_t> kSamples(4 * 32000, 1000);
    const SoundArchive archive =
        SequenceArchive({{0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xff},
                         {0xc7, 0x00, 0xc5, 0x0c, 0xc4, 0x7f, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff}},
                        {.wave = Rwav(4 * 32000, kSamples)});

    const Performance waited = PerformSound(archive, 0);
    const Performance stopped = PerformSound(archive, 1);

    REVOSF2_CHECK(waited.seconds > 2.0 && waited.seconds < 2.1 && !stopped.holds &&
                  std::fabs(stopped.seconds - waited.seconds) < 0.011);
}

void TestTimedSweepGlidesOnWhenTheSequenceStops()
{
    // With note wait off, hold key 60 with about 1 s of portamento from key 48, then set tempo 0. The sweep, timed in
    // milliseconds, goes on. The second sequence holds key 60, plays key 72 for a tick at release 127 with about 2 s of
    // portamento from key 48, and stops its timing at tick 1. Key 72 has ended by then. Its sweep stops too.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x17, 0x3c, 0x64, 0x00, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff},
                         {0xc7, 0x00, 0x3c, 0x64, 0x00, 0xd3, 0x7f, 0xc9, 0x30, 0xcf, 0x17,
                          0x48, 0x64, 0x01, 0x80, 0x01, 0xe1, 0x00, 0x00, 0x80, 0x30, 0xff}});

    const Performance gliding = PerformSound(archive, 0);
    const Performance ended = PerformSound(archive, 1);

    const std::vector<std::pair<uint32_t, float>> glide = Pitches(gliding);
    const std::vector<std::pair<uint32_t, float>> after_end = Pitches(ended);
    REVOSF2_CHECK(gliding.holds && glide.size() > 50 && glide.front().second == -12.0f && glide.back().second == 0.0f &&
                  glide.back().first < 100);
    REVOSF2_CHECK(ended.holds && !after_end.empty() && after_end.back().second == 0.0f && after_end.back().first <= 2);
}

// The places, in ticks, and the semitones of track `t`'s pitch events: a tick's at the start of its frame, and those
// between or after ticks where their frames start.
std::vector<std::pair<double, float>> PitchPlaces(const Performance& p, int t = 0)
{
    std::vector<std::pair<double, float>> pitches;
    for (const TrackEvent& e : p.tracks[t])
    {
        if (e.kind == EventKind::kPitch)
        {
            pitches.emplace_back(e.after > 0 ? e.tick + e.after : p.frame_ticks.at(e.tick), e.semitones);
        }
    }

    return pitches;
}

// True if `pitches` glide up 12 semitones in 120 ms with a pitch for each 3 ms sound frame. Each pitch sits where its
// frame starts at tempo 120: 0.096 ticks a millisecond.
bool GlidesEveryFrame(const std::vector<std::pair<double, float>>& pitches)
{
    if (pitches.size() != 41)
    {
        return false;
    }

    for (std::size_t f = 0; f < pitches.size(); f++)
    {
        const float semitones = -12.0f * static_cast<float>(120 - 3 * static_cast<int>(f)) / 120.0f;
        const double place = pitches[0].first + static_cast<double>(f) * 3.0 * 0.096;
        if (std::fabs(pitches[f].first - place) > 1e-4 || pitches[f].second != semitones)
        {
            return false;
        }
    }

    return true;
}

void TestTimedSweepMovesEveryFrame()
{
    // With note wait off, hold key 60 with 120 ms of portamento from key 48 (time 8). A tick at tempo 120 takes 10.4
    // ms, over three of the game's 3 ms sound frames, and the sweep moves on in each: every frame from the note's to
    // the one in which it reaches the key sends a pitch, placed where the frame starts.
    const Performance p = PerformCommands({0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0x3c, 0x64, 0x00, 0x80, 0x30, 0xff});

    const std::vector<std::pair<double, float>> pitches = PitchPlaces(p);

    REVOSF2_CHECK(GlidesEveryFrame(pitches) && !pitches.empty() && pitches[0].first == 0.0);
}

void TestTimedSweepGoesOnAfterItsTrackCloses()
{
    // Track 0 opens track 1 and waits 48 ticks. Track 1 holds key 60 at release 100 with 120 ms of portamento from key
    // 48. Track 1 closes 2 ticks later. That releases the note. The tail goes on gliding to the key. The game's
    // channels do the same. The second sequence is track 1's alone. Track 1 ends the sequence when it closes, and the
    // tail's pitches go on after the last tick.
    const std::vector<uint8_t> kGlide = {0xc7, 0x00, 0xc9, 0x30, 0xcf, 0x08, 0xd3,
                                         0x64, 0x3c, 0x64, 0x00, 0x80, 0x02, 0xff};
    std::vector<uint8_t> with_track_0 = {0x88, 0x01, 0x00, 0x00, 0x08, 0x80, 0x30, 0xff};
    with_track_0.insert(with_track_0.end(), kGlide.begin(), kGlide.end());
    const SoundArchive archive = SequenceArchive({with_track_0, kGlide}, {.track_flags = 3});

    const Performance closed = PerformSound(archive, 0);
    const Performance ended = PerformSound(archive, 1);

    REVOSF2_CHECK(GlidesEveryFrame(PitchPlaces(closed, 1)));
    const std::vector<std::pair<double, float>> after_end = PitchPlaces(ended);
    REVOSF2_CHECK(GlidesEveryFrame(after_end) && after_end.back().first > 2.0 && ended.end_tick >= 12);
}

void TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays()
{
    // Track 0 opens track 1 and then only waits 2 ticks and jumps back. Track 1 plays key 60 every 48 ticks, 30 times,
    // for 15 s, and ends. Track 0's passes change nothing while track 1 plays. They aren't passes of the main loop
    // until track 1 has ended. The sequence plays all of track 1. The idle loop after it sounds nothing (SilentLoop).
    // The sequence therefore ends with track 1's last note and has no loop markers.
    const Performance p = PerformCommands({0x88, 0x01, 0x00, 0x00, 0x0b, 0x80, 0x02, 0x89, 0x00, 0x00, 0x05,
                                           0xc7, 0x00, 0xd4, 0x1e, 0x3c, 0x64, 0x18, 0x80, 0x30, 0xfc, 0xff},
                                          {.track_flags = 3});

    REVOSF2_CHECK(NoteOns(p, 1).size() == 30 && !p.loop && p.loops == 0 && p.seconds > 14.75 && p.seconds < 15.0);
}

void TestTrackSettingAVariableAnotherReadsOwnsTheLoop()
{
    // Track 0 opens track 1, then waits 48 ticks and sets player variable 0 to 1, over and over. Track 1 checks the
    // variable every tick. When the variable is set, track 1 clears it and plays key 60. Track 0 changes no sound, but
    // track 1 reads the variable that track 0 sets. Track 0's 48-tick passes are then passes of the main loop.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x10, 0x80, 0x30, 0xf0, 0x80, 0x00, 0x00, 0x01, 0x89, 0x00,
                         0x00, 0x05, 0xc7, 0x00, 0x80, 0x01, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00,
                         0x00, 0x12, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0x89, 0x00, 0x00, 0x12},
                        {.track_flags = 3});

    std::vector<uint32_t> note_ons;
    for (const TrackEvent& e : p.tracks[1])
    {
        if (e.kind == EventKind::kNoteOn)
        {
            note_ons.push_back(e.tick);
        }
    }

    const bool every_48 = std::ranges::all_of(note_ons, [](uint32_t tick) { return tick % 48 == 0; });
    REVOSF2_CHECK(p.warnings.empty() && note_ons.size() == 19 && every_48);
    REVOSF2_CHECK(p.loop && p.loop->end - p.loop->start == 48);
}

void TestVariableSetAfterAJumpBackBelongsToTheNewPass()
{
    // As in TestTrackSettingAVariableAnotherReadsOwnsTheLoop, but track 0 sets the variable first in each pass, in the
    // tick it jumps back. That change belongs to the pass that the jump starts. Track 0's 48-tick passes still own the
    // main loop, not track 1's one-tick polls.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x10, 0xf0, 0x80, 0x00, 0x00, 0x01, 0x80, 0x30, 0x89, 0x00,
                         0x00, 0x05, 0xc7, 0x00, 0x80, 0x01, 0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x89, 0x00,
                         0x00, 0x12, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0x89, 0x00, 0x00, 0x12},
                        {.track_flags = 3});

    REVOSF2_CHECK(p.loop && p.loop->end - p.loop->start == 48);
}

void TestTrackWaitingInSilenceDoesNotHoldOffTheLoop()
{
    // Track 0 opens track 1, holds key 60 for 2,000 ticks with note wait off, and then only waits 2 ticks and jumps
    // back. Track 1 plays key 60 for 24 ticks, then waits 3,000 ticks (31 s) in silence and ends. A track that has
    // sounded nothing and changed nothing for 10 s doesn't count as playing on. Track 0's idle passes count from then.
    // The sequence stops about 10 s after track 1's note. Its loop is one of those passes. Track 0's note still sounds
    // there. The loop therefore isn't silent (SilentLoop).
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x11, 0xc7, 0x00, 0x3c, 0x64, 0x8f, 0x50, 0x80, 0x02,
                         0x89, 0x00, 0x00, 0x0b, 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x97, 0x38, 0xff},
                        {.track_flags = 3});

    REVOSF2_CHECK(p.seconds < 15.0 && p.loop && p.loop->start > 24 && p.loop->end - p.loop->start == 2);
}

void TestPollAfterTheSoundIsNoLoop()
{
    // Track 0 opens track 1, then checks each tick whether variable 0 is 1, and leaves the loop when it is. Nothing
    // sets the variable. Track 1 plays key 60 for 12 ticks and ends. Once the note has ended, track 0's passes count as
    // the main loop. They sound nothing (SilentLoop). The performance therefore ends with the note and has no loop
    // markers. When track 1 holds a note of 9,600 ticks instead, the same passes are the main loop under the note.
    const std::vector<uint8_t> root = {0x88, 0x01, 0x00, 0x00, 0x16, 0x80, 0x01, 0xf0, 0x90, 0x00, 0x00,
                                       0x01, 0xa2, 0x89, 0x00, 0x00, 0x15, 0x89, 0x00, 0x00, 0x05, 0xff};
    std::vector<uint8_t> one_shot = root;
    one_shot.insert(one_shot.end(), {0x3c, 0x64, 0x0c, 0xff});
    std::vector<uint8_t> held = root;
    held.insert(held.end(), {0x3c, 0x64, 0xcb, 0x00, 0xff});

    const Performance after = PerformCommands(one_shot, {.track_flags = 3});
    const Performance under = PerformCommands(held, {.track_flags = 3});

    REVOSF2_CHECK(!after.loop && after.loops == 0 && !after.truncated && NoteCount(after) == 1);
    REVOSF2_CHECK(after.seconds < 1.0);
    REVOSF2_CHECK(under.loop && under.loop->end == under.loop->start + 1 && under.seconds > 9.0);
}

void TestSilentPassesThatChangeSettingsEndWithTheSound()
{
    // Play key 60 for 12 ticks, then set the volume to 127 and to 64 in passes of 48 ticks. The passes change the track
    // and sound nothing (SilentLoop). The performance ends with the note. The volume changes after it are left out.
    const Performance p =
        PerformCommands({0x3c, 0x64, 0x0c, 0xc1, 0x7f, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, 0x00, 0x00, 0x03});

    const auto after_end = [&p](const TrackEvent& e)
    {
        return e.tick > p.end_tick;
    };
    REVOSF2_CHECK(!p.loop && p.loops == 0 && NoteCount(p) == 1 && p.seconds < 1.0);
    REVOSF2_CHECK(std::ranges::none_of(p.tracks[0], after_end));
}

void TestSequenceWithoutNotesEndsAtItsStart()
{
    // A sequence that only waits a tick and jumps back sounds nothing (SilentLoop). It ends at tick 0 with no loop
    // markers. Its MIDI file and SoundFont are still valid.
    const SoundArchive archive = SequenceArchive({{0x80, 0x01, 0x89, 0x00, 0x00, 0x00}});

    const Conversion c = ConvertSequence(archive, 0);

    const MidiView m = ParseMidi(c.midi);
    const auto at_start = [](uint64_t end)
    {
        return end == 0;
    };
    REVOSF2_CHECK(!c.performance.loop && c.performance.loops == 0 && c.performance.end_tick == 0);
    REVOSF2_CHECK(c.performance.seconds == 0.0 && !c.performance.truncated);
    REVOSF2_CHECK(m.ok && !m.track_ends.empty() && std::ranges::all_of(m.track_ends, at_start));
    REVOSF2_CHECK(Sf2Problem(c.sf2).empty());
}

void TestLoopStartSendsTheVolumeTheGameRestores()
{
    // With note wait off, set volume 100, play key 60 for 6 ticks and wait 12. The loop then sets volume 100 again,
    // plays key 60, and after half its length sets volume 50. The first pass requires no volume event at its start, but
    // the game goes back to 100 there. The loop start therefore sends it for a player that loops on the markers. The
    // first loop is 48 ticks long and passes on to the next pass; the second is 10 s long, and the performance stops at
    // its end.
    const SoundArchive archive =
        SequenceArchive({{0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c,
                          0x64, 0x18, 0x80, 0x18, 0xc1, 0x32, 0x80, 0x18, 0x89, 0x00, 0x00, 0x09},
                         {0xc7, 0x00, 0xc1, 0x64, 0x3c, 0x64, 0x06, 0x80, 0x0c, 0xc1, 0x64, 0x3c, 0x64,
                          0x18, 0x80, 0x83, 0x60, 0xc1, 0x32, 0x80, 0x83, 0x60, 0x89, 0x00, 0x00, 0x09}});

    const Performance short_loop = PerformSound(archive, 0);
    const Performance long_loop = PerformSound(archive, 1);

    auto sent_at_start = [](const Performance& p)
    {
        std::vector<uint8_t> at_zero;
        std::vector<uint8_t> at_start;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kVolume && e.tick == 0)
            {
                at_zero.push_back(e.value);
            }
            else if (e.kind == EventKind::kVolume && p.loop && e.tick == p.loop->start)
            {
                at_start.push_back(e.value);
            }
        }

        return p.loop && p.loop->start == 12 && at_zero.size() == 1 && at_start == at_zero;
    };
    REVOSF2_CHECK(sent_at_start(short_loop));
    REVOSF2_CHECK(sent_at_start(long_loop) && long_loop.loop->end == 972);
}

void TestLoopMarksAPassThatPlaysAsTheNext()
{
    // With note wait off, set volume 127 before the loop. The loop plays key 60, sets volume 64 halfway through and
    // jumps back. Its first pass plays the note at 127 and later passes at 64. The first pass sends 127 at its start. A
    // player that loops on that pass therefore plays every repeat too loud. The second pass plays as the next does. In
    // the first sequence a pass is 48 ticks, and the performance runs on to 10 s anyway. In the second a pass is 10 s.
    // The second pass plays as the third. The performance therefore plays the second pass there too.
    const SoundArchive archive = SequenceArchive(
        {{0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x18, 0xc1, 0x40, 0x80, 0x18, 0x89, 0x00, 0x00, 0x04},
         {0xc7, 0x00, 0xc1, 0x7f, 0x3c, 0x64, 0x18, 0x80, 0x83, 0x60, 0xc1, 0x40, 0x80, 0x83, 0x60, 0x89, 0x00, 0x00,
          0x04}});

    const Performance short_loop = PerformSound(archive, 0);
    const Performance long_loop = PerformSound(archive, 1);

    REVOSF2_CHECK(short_loop.loop && short_loop.loop->start == 48 && short_loop.loop->end == 96);
    REVOSF2_CHECK(long_loop.loop && long_loop.loop->start == 960 && long_loop.loop->end == 1920 &&
                  long_loop.loops == 1 && long_loop.seconds > 20.0 && long_loop.seconds < 21.0);
}

void TestLoopDoesNotStrikeATiedNoteAgain()
{
    // Turn tie on. The loop ties key 60 for a tick, pans left, ties it again for a tick, pans right and jumps back. The
    // first pass strikes the note, and later passes only tie it again. The loop is therefore the second pass. A player
    // repeats that pass without striking the note every 2 ticks.
    const Performance p = PerformCommands(
        {0xc8, 0x01, 0x3c, 0x64, 0x01, 0xc0, 0x20, 0x3c, 0x64, 0x01, 0xc0, 0x60, 0x89, 0x00, 0x00, 0x02});

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    REVOSF2_CHECK(note_ons == 1 && p.loop && p.loop->start == 2 && p.loop->end == 4);
}

void TestLoopThatNoPassRepeatsKeepsItsFirstPass()
{
    // Track 0 opens track 1, then plays key 60 every 960 ticks (10 s); track 1 plays key 64 every 959. Track 1's notes
    // come a tick earlier in each of track 0's passes. No pass therefore plays as the next. The loop stays on the first
    // pass, and the performance doesn't play another pass to find one.
    const Performance p =
        PerformCommands({0x88, 0x01, 0x00, 0x00, 0x11, 0xc7, 0x00, 0x3c, 0x64, 0x18, 0x80, 0x87, 0x40, 0x89, 0x00,
                         0x00, 0x07, 0xc7, 0x00, 0x40, 0x64, 0x18, 0x80, 0x87, 0x3f, 0x89, 0x00, 0x00, 0x13},
                        {.track_flags = 3});

    REVOSF2_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 960 && p.seconds < 12.0);
}

void TestPassOnAnotherPartPlaysAsTheNext()
{
    // With attack and release 122, glide from key 67 to key 83 over 30 ticks, wait 10 and jump back. Each note glides
    // while the last one's release still sounds. The notes therefore take turns on two parts, and a part that a note
    // moves to takes every setting again. The note of a pass and the note of the next pass are on different parts with
    // the same settings. So the first pass plays as the next.
    const Performance p =
        PerformCommands({0xd0, 0x7a, 0xd3, 0x7a, 0xc9, 0x43, 0x53, 0x5a, 0x1e, 0x80, 0x0a, 0x89, 0x00, 0x00, 0x00});

    const std::vector<int> parts = NoteParts(p);
    REVOSF2_CHECK(parts.size() > 3 && parts[0] == 0 && parts[1] == 1 && parts[2] == 0);
    REVOSF2_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 40);
}

void TestSequenceThatFinishesHasNoLoop()
{
    // Set variable 0 to 0. Play key 60 for 24 ticks, add 1 to the variable, and jump back while it's below 3, then end.
    // The passes count as a loop on the way, but the sequence ends after three notes. It therefore has no loop.
    const Performance p = PerformCommands({0xf0, 0x80, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xf0, 0x81, 0x00, 0x00,
                                           0x01, 0xf0, 0x94, 0x00, 0x00, 0x03, 0xa2, 0x89, 0x00, 0x00, 0x05, 0xff});

    const auto note_ons =
        std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });

    REVOSF2_CHECK(note_ons == 3 && !p.loop && p.loops == 0);
}

void TestVariableOptionSetsAVariableBeforeTheStart()
{
    // Wait a tick and check again while player variable 0 isn't 1, then play key 60 and end. Variables start at -1. The
    // sequence therefore plays nothing until the game sets the variable. PerformOptions::variables sets it first.
    const SoundArchive archive = SequenceArchive(
        {{0xf0, 0x95, 0x00, 0x00, 0x01, 0xa2, 0x80, 0x01, 0xa2, 0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x18, 0xff}});
    BankSet banks(archive, archive.Sounds()[0]);
    PerformOptions options;
    options.variables[0] = 1;

    const Performance unset = Perform(archive, 0, banks, {});
    const Performance set = Perform(archive, 0, banks, options);

    auto note_ons = [](const Performance& p)
    {
        return std::ranges::count_if(p.tracks[0], [](const TrackEvent& e) { return e.kind == EventKind::kNoteOn; });
    };
    REVOSF2_CHECK(note_ons(unset) == 0 && note_ons(set) == 1);
}

void TestRetryWithoutAWaitIsNoLoop()
{
    // Draw variable 0 at random from 0 to 3. While it's 0, jump back without a wait and draw again. Then play a 48-tick
    // key 60 and jump back to the draw. The retries aren't passes of a loop. Each note's pass is.
    const Performance p = PerformCommands({0xf0, 0x86, 0x00, 0x00, 0x03, 0xf0, 0x90, 0x00, 0x00, 0x00, 0xa2,
                                           0x89, 0x00, 0x00, 0x00, 0x3c, 0x64, 0x30, 0x89, 0x00, 0x00, 0x00});

    REVOSF2_CHECK(p.loop && p.loop->start == 0 && p.loop->end == 48 && p.warnings.empty());
}

void TestVariablesStartAsTheGamesDo()
{
    // A player variable (0) and a track variable (32) start at -1; a global variable (16) is 0 until the game sets one.
    // Each comparison that holds plays a note.
    const Performance p =
        PerformCommands({0xf0, 0x90, 0x00, 0xff, 0xff, 0xa2, 0x3c, 0x64, 0x30, 0xf0, 0x90, 0x10, 0x00, 0x00,
                         0xa2, 0x3e, 0x64, 0x30, 0xf0, 0x90, 0x20, 0xff, 0xff, 0xa2, 0x40, 0x64, 0x30, 0xff});

    const std::vector<std::tuple<uint32_t, int, int>> kNotes = {{0, 60, 100}, {48, 62, 100}, {96, 64, 100}};
    REVOSF2_CHECK(NoteOns(p) == kNotes);
}

void TestTrackStartsNoMoreNotesATickThanTheGameHasVoices()
{
    // AX has 96 voices. With note wait off, 100 key-60 notes in a tick start 96. With tie on, 97 notes and then key 72
    // play on one channel. The channel's last legato, the 97th, requires no approximation. Program 5 doesn't exist, and
    // 96 notes on it take no voices. A held note on program 0 after them still plays. And 97 tied notes at velocity 1
    // are followed by one at 127 with a new release. That release would require another preset. The MIDI note continues
    // in its preset and at its level instead. The whole sequence doesn't get quieter.
    const SoundArchive archive = SequenceArchive(
        {{0xc7, 0x00, 0xd4, 0x64, 0x3c, 0x64, 0x30, 0xfc, 0x80, 0x30, 0xff},
         {0xc7, 0x00, 0xc8, 0x01, 0xd4, 0x61, 0x3c, 0x64, 0x30, 0xfc, 0x48, 0x64, 0x30, 0x80, 0x30, 0xff},
         {0xc7, 0x00, 0x81, 0x05, 0xd4, 0x60, 0x3c, 0x64, 0x30, 0xfc, 0x81, 0x00, 0xc7, 0x01, 0x3c, 0x64, 0x00, 0xff},
         {0xc7, 0x00, 0xc8, 0x01, 0x3c, 0x01, 0x30, 0xd4, 0x60, 0x3c, 0x01,
          0x30, 0xfc, 0xd3, 0x40, 0x3c, 0x7f, 0x30, 0x80, 0x30, 0xff}});

    const Performance flood = PerformSound(archive, 0);
    const Performance tied = PerformSound(archive, 1);
    const Performance held = PerformSound(archive, 2);
    const Performance loud = PerformSound(archive, 3);

    const std::string kLegatosPastCap =
        "more than 96 legatos in a tick (keeping the MIDI preset and limiting gain to the starting level)";
    const auto left_out = flood.approximations.find(
        "more than 96 new channels on a track in one tick (excess omitted; the game keeps the newest)");
    const std::vector<std::pair<uint32_t, float>> tied_pitch = Pitches(tied);
    REVOSF2_CHECK(NoteOns(flood).size() == 96 && left_out != flood.approximations.end() && left_out->second == 4);
    REVOSF2_CHECK(NoteOns(tied).size() == 1 && !tied_pitch.empty() && tied_pitch.back().second == 12.0f &&
                  !tied.approximations.contains(kLegatosPastCap));
    REVOSF2_CHECK(NoteOns(held).size() == 1 && held.holds);
    REVOSF2_CHECK(NoteOns(loud).size() == 1 && loud.level_cut_db == 0.0 &&
                  loud.approximations.contains(kLegatosPastCap));
}

void TestTrackPlayingNotesWithoutWaitingIsStopped()
{
    // Loop a one-tick key 60 with note wait off. The track never waits. That would hang the game. The performer stops
    // the track in its first tick. Its notes are released before they've sounded.
    const Performance p = PerformCommands({0xc7, 0x00, 0x3c, 0x64, 0x01, 0x89, 0x00, 0x00, 0x02});

    const std::vector<std::string> kStopped = {"track 0 stopped after running without waits"};
    REVOSF2_CHECK(p.warnings == kStopped && NoteCount(p) == 0 && p.end_tick <= 1);
}

void TestSoundsPanCurveAndModeAreReported()
{
    // Pan a note left with four sound settings: pan curve 2, balance mode with a mono wave (which it doesn't affect),
    // balance mode with a stereo wave, and the default curve and mode.
    std::vector<ArchiveFile> files(3);
    files[0].data = Rseq({0xc0, 0x20, 0x3c, 0x64, 0x30, 0xff});
    files[1].data = Rbnk({TestInstrument{}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 1, true)});
    files[2].data = Rbnk({TestInstrument{}});
    files[2].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 2, true)});
    const std::vector<SequenceSound> kSounds = {
        {.bank = 0, .pan_curve = 2}, {.bank = 0, .pan_mode = 1}, {.bank = 1, .pan_mode = 1}, {.bank = 0}};
    const SoundArchive archive = SoundArchive::Load(Rsar(files, {.sequences = kSounds, .banks = {1, 2}}));

    const Performance curve = PerformSound(archive, 0);
    const Performance mono_balance = PerformSound(archive, 1);
    const Performance stereo_balance = PerformSound(archive, 2);
    const Performance usual = PerformSound(archive, 3);

    const std::string kCurve = "custom pan curve (using the default)";
    const std::string kBalance = "track pan on stereo waves in balance mode (they pan as in dual mode)";
    REVOSF2_CHECK(curve.approximations.contains(kCurve) && !curve.approximations.contains(kBalance));
    REVOSF2_CHECK(stereo_balance.approximations.contains(kBalance) && !stereo_balance.approximations.contains(kCurve));
    REVOSF2_CHECK(mono_balance.approximations.empty() && usual.approximations.empty());
}

void TestTempoAndSweepFollowTheBuild()
{
    // Tempo 2047 and a sweep of 64. Builds from 2008 keep tempos to 1023 and give the sweep in 64ths of a semitone;
    // older builds take the tempo as it is and the sweep in 32nds.
    const std::vector<uint8_t> kCommands = {0xe1, 0x07, 0xff, 0xe3, 0x00, 0x40, 0x3c, 0x64, 0x30, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    const std::vector<std::pair<uint32_t, float>> late_sweep = Pitches(late), early_sweep = Pitches(early);
    REVOSF2_CHECK(late.since_2008 && !early.since_2008);
    REVOSF2_CHECK(late.tempo.back().bpm == 1023.0 && early.tempo.back().bpm == 2047.0);
    REVOSF2_CHECK(!late_sweep.empty() && late_sweep.front().second == 1.0f);
    REVOSF2_CHECK(!early_sweep.empty() && early_sweep.front().second == 2.0f);
}

void TestMonophonicAndVelocityRangeComeWith2008()
{
    // Monophonic on, velocity range 64, note wait off, and keys 60 and 62 at velocity 100. Builds from 2008 play one
    // channel, at velocity 100 * 64 / 127. That channel takes the second key as a legato. Older builds lack both
    // commands and play both notes at velocity 100.
    const std::vector<uint8_t> kCommands = {0xb2, 0x01, 0xb3, 0x40, 0xc7, 0x00, 0x3c, 0x64,
                                            0x30, 0x3e, 0x64, 0x30, 0x80, 0x60, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    const std::vector<std::tuple<uint32_t, int, int>> kLate = {{0, 60, 50}};
    const std::vector<std::tuple<uint32_t, int, int>> kEarly = {{0, 60, 100}, {0, 62, 100}};
    REVOSF2_CHECK(NoteOns(late) == kLate && NoteOns(early) == kEarly);
}

void TestRegionsNoteOffTypeAndKeyGroupComeWith2008()
{
    // A region that ignores note-offs, with decay 100 and sustain 0, plays key 60 for one tick. In builds from 2008,
    // the note sounds on, and its MIDI note ends where the note has faded 60 dB (kFadedAttenuation, at about 1.31 s).
    // Older builds release it at tick 1. A region of key group 1 plays key 60 and, 24 ticks later, key 62. In builds
    // from 2008, key 62 releases key 60.
    const std::vector<uint8_t> kShort = {0x3c, 0x64, 0x01, 0x80, 0x87, 0x40, 0xff};
    const std::vector<uint8_t> kTwoKeys = {0xc7, 0x00, 0x3c, 0x64, 0x60, 0x80, 0x18,
                                           0x3e, 0x64, 0x60, 0x80, 0x60, 0xff};
    const TestRegion kIgnoring = {.decay = 100, .sustain = 0, .note_off_type = 1};
    const TestRegion kGrouped = {.key_group = 1};

    const Performance ignoring = PerformCommands(kShort, {.region = kIgnoring});
    const Performance releasing = PerformCommands(kShort, {.version = 0x0103, .region = kIgnoring});
    const Performance grouped = PerformCommands(kTwoKeys, {.region = kGrouped});
    const Performance ungrouped = PerformCommands(kTwoKeys, {.version = 0x0103, .region = kGrouped});

    const std::vector<std::pair<uint32_t, EventKind>> ignored = NoteEvents(ignoring);
    const std::vector<std::pair<uint32_t, EventKind>> kReleased = {{0, EventKind::kNoteOn}, {1, EventKind::kNoteOff}};
    const EnvelopeValues kEnvelope{127, 100, 0, 0, 127};
    const int64_t faded_frames = FramesToLevel(kEnvelope, -static_cast<float>(kFadedAttenuation));
    const double faded_ms = static_cast<double>(faded_frames + 1) * kFrameMs;
    REVOSF2_CHECK(ignored.size() == 2 && ignored[1].first * kTickMs >= faded_ms &&
                  ignored[1].first * kTickMs < faded_ms + kTickMs + kFrameMs);
    REVOSF2_CHECK(NoteEvents(releasing) == kReleased);
    const std::vector<std::pair<uint32_t, EventKind>> kGroupedNotes = {
        {0, EventKind::kNoteOn}, {24, EventKind::kNoteOff}, {24, EventKind::kNoteOn}, {120, EventKind::kNoteOff}};
    const std::vector<std::pair<uint32_t, EventKind>> kUngroupedNotes = {
        {0, EventKind::kNoteOn}, {24, EventKind::kNoteOn}, {96, EventKind::kNoteOff}, {120, EventKind::kNoteOff}};
    REVOSF2_CHECK(NoteEvents(grouped) == kGroupedNotes && NoteEvents(ungrouped) == kUngroupedNotes);
}

// A DOL with AX's microcode in its first data section: two NOPs and seven jumps to 0x0Fxx, then a DMA of the parameter
// blocks by `block_size` bytes. `banner` follows the section.
std::vector<uint8_t> TestDol(uint16_t block_size, const std::string& banner)
{
    Writer w;
    w.Add(0x100);
    const std::size_t code = w.Add(0x2000);
    w.U32(4 * 7, static_cast<uint32_t>(code));
    w.U32(0x48 + 4 * 7, 0x80004000);
    w.U32(0x90 + 4 * 7, 0x2000);

    for (std::size_t i = 0; i < 7; i++)
    {
        w.U16(code + 4 + 4 * i, 0x029f);
        w.U16(code + 6 + 4 * i, static_cast<uint32_t>(0x0f00 + 0x10 * i));
    }

    w.U16(code + 0x100, 0x16cb);
    w.U16(code + 0x102, block_size);
    w.Add(std::span(reinterpret_cast<const uint8_t*>(banner.data()), banner.size()));

    return w.bytes;
}

void TestMainDolShowsTheBuild()
{
    // AX of October 2006 transfers 0x120-byte parameter blocks. It comes with NW4R SND's build of 2006. That build has
    // no banner. A later AX transfers 0x140 bytes, and NW4R SND's banner dates the build. A file without AX's microcode
    // shows nothing.
    const std::vector<uint8_t> kNoMicrocode(0x3000, 0);

    const std::optional<SndBuild> old_build = ReadSndBuild(TestDol(0x120, ""));
    const std::optional<SndBuild> march =
        ReadSndBuild(TestDol(0x140, "<< NW4R    - SND \tfinal   build: Mar  8 2008 10:59:21 (0x4302_145) >>"));
    const std::optional<SndBuild> june =
        ReadSndBuild(TestDol(0x140, "<< NW4R    - SND \tfinal   build: Jun 20 2008 11:01:02 (0x4302_158) >>"));
    const std::optional<SndBuild> no_microcode = ReadSndBuild(kNoMicrocode);
    const std::optional<SndBuild> empty = ReadSndBuild({});

    REVOSF2_CHECK(old_build && old_build->ax_2006 && old_build->date == 0 && !old_build->Since2008());
    REVOSF2_CHECK(march && !march->ax_2006 && march->date == 20080308 && march->Since2008() && !march->SinceJune2008());
    REVOSF2_CHECK(june && !june->ax_2006 && june->date == 20080620 && june->SinceJune2008());
    REVOSF2_CHECK(!no_microcode && !empty);
}

void TestMainDolIsFoundBesideTheFiles()
{
    // Dolphin and wit extract a disc into a sys folder and a files folder. An archive anywhere in files finds
    // sys/main.dol. An archive outside a files folder finds nothing.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "revosf2_test_disc";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "files" / "Sound");
    std::filesystem::create_directories(root / "sys");
    for (const std::filesystem::path& file :
         {root / "sys" / "main.dol", root / "files" / "Sound" / "a.brsar", root / "a.brsar"})
    {
        std::ofstream(file, std::ios::binary).put('\0');
    }

    const std::optional<std::filesystem::path> inside = FindMainDol(root / "files" / "Sound" / "a.brsar");
    const std::optional<std::filesystem::path> outside = FindMainDol(root / "a.brsar");
    std::filesystem::remove_all(root);

    REVOSF2_CHECK(inside && inside->filename() == "main.dol" && inside->parent_path().filename() == "sys");
    REVOSF2_CHECK(!outside);
}

void TestBuildOf2006CountsTicksIn416ths()
{
    // The build of October 2006 adds the tempo to a count of 416ths of a tick each 3 ms frame, and runs the whole
    // ticks. At tempo 120, tick 0 runs in frame 0, tick 1 in frame 4 (12 ms) and tick 2 in frame 7 (21 ms). MIDI's
    // tempo of 120 * 20000 / 416 / 48 beats a minute puts the ticks at 10.4 and 20.8 ms. Their events go to the frames'
    // starts, 1.6 and 0.2 ms later. The build of 2007 runs a tick every 10.4 ms. Its events go to the starts of frames
    // 3 and 6, before the ticks.
    const std::vector<uint8_t> kCommands = {0x3c, 0x64, 0x01, 0x3e, 0x64, 0x01, 0x40, 0x64, 0x01, 0xff};
    const SndBuild kOctober2006{.ax_2006 = true};
    const double kOctoberTickMs = 3.0 * 416.0 / 120.0;

    const Performance old_build = PerformCommands(kCommands, {.version = 0x0101, .build = kOctober2006});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    REVOSF2_CHECK(old_build.tempo.size() == 1 &&
                  std::abs(old_build.tempo[0].bpm - 120.0 * 20000.0 / 416.0 / 48.0) < 1e-9);
    REVOSF2_CHECK(old_build.frame_ticks.size() >= 3 && old_build.frame_ticks[0] == 0.0 &&
                  std::abs(old_build.frame_ticks[1] - (1.0 + (12.0 - kOctoberTickMs) / kOctoberTickMs)) < 1e-9 &&
                  std::abs(old_build.frame_ticks[2] - (2.0 + (21.0 - 2.0 * kOctoberTickMs) / kOctoberTickMs)) < 1e-9);
    REVOSF2_CHECK(later.frame_ticks.size() >= 3 && later.frame_ticks[1] < 1.0 && later.frame_ticks[2] < 2.0);
}

void TestBuildOf2006HasNoTimebase()
{
    // Timebase 96 at tick 0. The build of October 2006 has no timebase, and MIDI keeps 48 ticks a quarter note. The
    // build of 2007 gives MIDI the timebase.
    const std::vector<uint8_t> kCommands = {0xb0, 0x60, 0x3c, 0x64, 0x30, 0xff};

    const Performance old_build = PerformCommands(kCommands, {.version = 0x0101, .build = SndBuild{.ax_2006 = true}});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    REVOSF2_CHECK(old_build.timebase == 48 && later.timebase == 96);
}

void TestBuildOf2006RaisesPan1AndHasOnlyDualMode()
{
    // Pan 1. The build of October 2006 raises it to 2: -62/63 of the way to the left. The build of 2007 pans it hard
    // left. The build of October 2006 also plays every stereo wave in dual mode, where the build of 2007 plays an RSAR
    // 1.1 archive's in balance mode.
    auto pans = [](const Performance& p)
    {
        std::vector<int> values;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kPan)
            {
                values.push_back(e.value);
            }
        }

        return values;
    };
    const std::vector<uint8_t> kPan1 = {0xc0, 0x01, 0x3c, 0x64, 0x30, 0xff};
    const std::vector<uint8_t> kPan2 = {0xc0, 0x02, 0x3c, 0x64, 0x30, 0xff};

    const Performance old_build = PerformCommands(kPan1, {.version = 0x0101, .build = SndBuild{.ax_2006 = true}});
    const Performance later = PerformCommands(kPan1, {.version = 0x0101});
    const Performance raised = PerformCommands(kPan2, {.version = 0x0101});

    REVOSF2_CHECK(!pans(old_build).empty() && pans(old_build) == pans(raised) && pans(old_build) != pans(later));
    REVOSF2_CHECK(!old_build.balance && later.balance);
}

void TestBuildOf2006StopsAReleaseAtVolume0()
{
    // Release 64, and key 60 for one tick. Two ticks after the release, the track's volume falls to 0. The build of
    // October 2006 stops the released note there: its volume times its envelope is 0. MIDI cuts its release tail. In
    // the build of 2007, the tail runs on in silence.
    const std::vector<uint8_t> kCommands = {0xd3, 0x40, 0x3c, 0x64, 0x01, 0x80, 0x02, 0xc1, 0x00, 0x80, 0x60, 0xff};
    auto cuts = [](const Performance& p)
    {
        auto cut = [](const TrackEvent& e)
        {
            return e.kind == EventKind::kSoundOff;
        };
        return std::ranges::count_if(p.tracks[0], cut);
    };

    const Performance old_build = PerformCommands(kCommands, {.version = 0x0101, .build = SndBuild{.ax_2006 = true}});
    const Performance later = PerformCommands(kCommands, {.version = 0x0101});

    REVOSF2_CHECK(cuts(old_build) == 1 && cuts(later) == 0);
}

void TestBiquadSwitchedLaterInTheFrameReachesItsNotes()
{
    // At timebase 96 and tempo 999 a tick lasts 0.63 ms. Ticks 0 and 1 then share a 3 ms sound frame. With note wait
    // off, play key 60 at tick 0 and switch the biquad on (low-pass, 64) at tick 1. Or start with it on and switch it
    // off at tick 1. The game gives the note the biquad that the frame ends with. The SFZ file selects the note's
    // region at note-on. That value must come before the note. Switched on 10 ticks later, in another frame, the biquad
    // doesn't reach the note.
    auto biquad_at_note_on = [](const std::vector<uint8_t>& switching)
    {
        std::vector<uint8_t> commands = {0xb0, 0x60, 0xe1, 0x03, 0xe7, 0xc7, 0x00};
        commands.insert(commands.end(), switching.begin(), switching.end());
        commands.insert(commands.end(), {0x80, 0x60, 0xff});
        const Performance p = PerformCommands(commands);

        int value = 0;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kNoteOn)
            {
                return value;
            }

            if (e.kind == EventKind::kSfzBiquad)
            {
                value = e.value;
            }
        }

        return -1;
    };

    const int on = biquad_at_note_on({0x3c, 0x64, 0x30, 0x80, 0x01, 0xb4, 0x01, 0xb5, 0x40});
    const int off = biquad_at_note_on({0xb4, 0x01, 0xb5, 0x40, 0x3c, 0x64, 0x30, 0x80, 0x01, 0xb5, 0x00});
    const int later = biquad_at_note_on({0x3c, 0x64, 0x30, 0x80, 0x0a, 0xb4, 0x01, 0xb5, 0x40});

    REVOSF2_CHECK(on == SfzBiquadValue(1, 64) && off == 0 && later == 0);
}

void TestBuildsFrom2008PlayTheBiquad()
{
    // A biquad filter (type 1, value 64) plays in builds from 2008: the SoundFont preset takes the biquad low-pass's
    // row 55, and the SFZ controller the value. Older builds ignore the commands. An envelope reset puts back the
    // region's release after the track's release 10.
    const std::vector<uint8_t> kCommands = {0xb4, 0x01, 0xb5, 0x40, 0xd3, 0x0a, 0xfb, 0x3c, 0x64, 0x30, 0xff};

    const Performance late = PerformCommands(kCommands);
    const Performance early = PerformCommands(kCommands, {.version = 0x0103});

    auto biquad_controller = [](const Performance& p)
    {
        int value = 0;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kSfzBiquad)
            {
                value = e.value;
            }
        }

        return value;
    };
    REVOSF2_CHECK(late.presets.size() == 1 && late.presets[0].release == 0xff);
    REVOSF2_CHECK(late.presets[0].filter == ToSoundFont(0x40, 1, 64) && late.presets[0].filter.resonance == 17);
    REVOSF2_CHECK(late.biquad_types[0] == 1 && biquad_controller(late) == 64);
    REVOSF2_CHECK(early.presets.size() == 1 && early.presets[0].release == 10);
    REVOSF2_CHECK(early.presets[0].filter == SoundFontFilter{} && early.biquad_types[0] == 0);
}

// The preset event in front of the first note-on of `p`'s track 0.
const PresetKey* FirstNotePreset(const Performance& p)
{
    const PresetKey* preset = nullptr;
    for (const TrackEvent& e : p.tracks[0])
    {
        if (e.kind == EventKind::kPreset)
        {
            preset = &p.presets[e.preset];
        }
        else if (e.kind == EventKind::kNoteOn)
        {
            return preset;
        }
    }

    return nullptr;
}

void TestLfoSetAfterANoteInItsTickReachesItsPreset()
{
    // With note wait off and LFO depth 40, play key 60, then set the LFO speed to 10 in the same tick. The game gives
    // the note the track's LFO settings at the end of the sound frame. The note's preset has speed 10, and nothing is
    // reported.
    const Performance p = PerformCommands({0xc7, 0x00, 0xca, 0x28, 0x3c, 0x64, 0x60, 0xcb, 0x0a, 0x80, 0x60, 0xff});

    const PresetKey* preset = FirstNotePreset(p);

    REVOSF2_CHECK(preset && preset->lfo_speed == 10 && p.approximations.empty());
}

void TestLfoChangedAtDepth0IsReportedWhenHeard()
{
    // With note wait off and LFO depth 0, play key 60 for 192 ticks. 48 ticks in, set the LFO speed to 10, and 48 ticks
    // later raise the depth to 40. The note's preset keeps speed 16. The game plays the note's LFO at speed 10. The
    // difference is reported once.
    const Performance p = PerformCommands({0xc7, 0x00, 0xca, 0x00, 0x3c, 0x64, 0x81, 0x40, 0x80, 0x30, 0xcb, 0x0a, 0x80,
                                           0x30, 0xca, 0x28, 0x80, 0x60, 0xff});

    const PresetKey* preset = FirstNotePreset(p);
    const auto moved = p.approximations.find("LFO parameters changed during a note (using its note-on settings)");

    REVOSF2_CHECK(preset && preset->lfo_speed == 16 && moved != p.approximations.end() && moved->second == 1);
}

void TestDeepTremoloIsKeptTo6dB()
{
    // A volume LFO (type 1) at depth 127 swings 6 dB per unit of range. The game can't raise a note more than 6 dB. At
    // range 2, CC1 drops from 64 to 32. That swings 6 dB in the SoundFont, and the cap is reported. At range 1, CC1
    // stays at 64. A pitch LFO at range 2 keeps 64 too.
    const SoundArchive archive = SequenceArchive({{0xcc, 0x01, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff},
                                                  {0xcc, 0x01, 0xcd, 0x01, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff},
                                                  {0xcc, 0x00, 0xcd, 0x02, 0xca, 0x7f, 0x3c, 0x64, 0x30, 0xff}});

    const Performance deep = PerformSound(archive, 0);
    const Performance shallow = PerformSound(archive, 1);
    const Performance vibrato = PerformSound(archive, 2);

    auto modulation = [](const Performance& p)
    {
        std::vector<int> values;
        for (const TrackEvent& e : p.tracks[0])
        {
            if (e.kind == EventKind::kModulation)
            {
                values.push_back(e.value);
            }
        }

        return values;
    };
    const char* kReport = "volume LFO deeper than 6 dB (limited to 6 dB: it dips less than in the game)";
    REVOSF2_CHECK(modulation(deep) == std::vector<int>{32} && deep.approximations.count(kReport) == 1);
    REVOSF2_CHECK(modulation(shallow) == std::vector<int>{64} && shallow.approximations.count(kReport) == 0);
    REVOSF2_CHECK(modulation(vibrato) == std::vector<int>{64} && vibrato.approximations.count(kReport) == 0);
}

void TestNoteReleasedInItsAttackKeepsTheGamesLevel()
{
    // With attack 20, key 60 plays for 5 ticks. The note is released 51 ms into its attack. The game is at -37.8 dB
    // then. The SoundFont attack, 331 ms long, has reached -16 dB. A second sequence plays the note for 200 ticks, past
    // the attack. A SoundFont attack that ramps up over 51 ms * 10^(-L / 20) reaches the game's level L dB at the
    // release. Only the first note gets such an attack.
    const SoundArchive archive =
        SequenceArchive({{0xd0, 0x14, 0x3c, 0x64, 0x05, 0xff}, {0xd0, 0x14, 0x3c, 0x64, 0x81, 0x48, 0xff}});

    const Performance early = PerformSound(archive, 0);
    const Performance late = PerformSound(archive, 1);

    EnvelopeValues env;
    env.attack = 20;
    const int64_t kFrames = 17; // 5 ticks of 10.4 ms, in 3 ms frames from the note-on frame
    const double level_db = HeldLevel(env, kFrames) / 10.0;
    const double attack_ms = static_cast<double>(kFrames) * kFrameMs * std::pow(10.0, -level_db / 20.0);
    REVOSF2_CHECK(level_db < -30.0 && attack_ms > Sf2AttackMs(env));
    REVOSF2_CHECK(early.presets.size() == 1 && early.presets[0].attack_timecents &&
                  *early.presets[0].attack_timecents == ToTimecents(attack_ms, -12000, 8000));
    REVOSF2_CHECK(late.presets.size() == 1 && !late.presets[0].attack_timecents);
}

void TestTrackWaitingForeverEnds()
{
    // After a note, a wait takes its length from player variable 0. The variable is -1, and a negative wait waits for
    // good. Once nothing sounds, the sequence is over. It doesn't run to the time limit.
    const Performance p = PerformCommands({0x3c, 0x64, 0x30, 0xa1, 0x80, 0x00, 0xff});

    REVOSF2_CHECK(!p.truncated && !p.holds && p.seconds < 1.0);
}

void TestHeldNoteHoldsUntilStopped()
{
    // Key 60 for 10,000 ticks with note wait on: 104 s at tempo 120. As WSF counts it, a note of 9,600 ticks or more is
    // one the game stops itself. The sequence holds and ends 10 s after it last changes. A note of 9,599 ticks plays
    // out.
    const SoundArchive archive = SequenceArchive({{0x3c, 0x64, 0xce, 0x10, 0xff}, {0x3c, 0x64, 0xca, 0x7f, 0xff}});

    const Performance held = PerformSound(archive, 0);
    const Performance played = PerformSound(archive, 1);

    REVOSF2_CHECK(held.holds && !held.truncated && held.seconds >= 10.0 && held.seconds < 10.1);
    REVOSF2_CHECK(!played.holds && played.seconds > 99.0);
}

void TestEnvelopeValuesMatchTheGame()
{
    REVOSF2_CHECK(AttackMs(127) == 1.0);
    REVOSF2_CHECK(AttackMs(0) > 13000.0 && AttackMs(0) < 13300.0); // 13.1 s: the slowest attack
    REVOSF2_CHECK(DecayRate(127) == 65535.0 && DecayRate(126) == 24.0);
    REVOSF2_CHECK(SustainCentibels(127) == 0 && SustainCentibels(0) == -723);
    REVOSF2_CHECK(HoldMs(0) == 0.0 && HoldMs(1) == 1.0 && HoldMs(127) == 4096.0);
}

void TestRegionsHoldComesWith2008()
{
    // Builds before 2008 have no hold, whatever the region gives.
    Region region;
    region.hold = 50;

    REVOSF2_CHECK(RegionEnvelope(region, true).hold == 50 && RegionEnvelope(region, false).hold == 0);
}

void TestInstantEnvelopeIsAnOrgan()
{
    // Attack, decay, sustain and release 127, hold 0: full level at once, and silence at once on release.
    const Sf2Envelope e = ConvertEnvelope(EnvelopeValues{127, 127, 127, 0, 127});

    REVOSF2_CHECK(e.attack == -12000 && e.hold == -12000 && e.sustain == 0 && e.release == -12000);
}

void TestDecayTakesTheGamesTime()
{
    // A decay of 100 dB takes 1000 / rate milliseconds.
    const double decay_ms = 1000.0 / DecayRate(100);

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 100, 64, 0, 100});

    REVOSF2_CHECK(std::abs(d.decay - 1200.0 * std::log2(decay_ms / 1000.0)) <= 1.0);
    REVOSF2_CHECK(d.sustain == -SustainCentibels(64));
}

void TestSlowDecaySplitsItsError()
{
    // Decay 2 is slower than SoundFont's slowest decay, 8000 timecents for 100 dB. The hold makes up half the
    // difference. At the hold's end, the note is as far above the game's level as the note is below that level when the
    // decay reaches sustain 100 (-4.2 dB).
    const double game_ms = 1000.0 / DecayRate(2);
    const double sustain_db = -SustainCentibels(100) / 10.0;

    const Sf2Envelope d = ConvertEnvelope(EnvelopeValues{127, 2, 100, 0, 127});

    const double hold_ms = 1000.0 * std::exp2(d.hold / 1200.0);
    const double reached_ms = hold_ms + sustain_db / 100.0 * 1000.0 * std::exp2(d.decay / 1200.0);
    const double loud_db = 100.0 * hold_ms / game_ms;
    const double quiet_db = sustain_db - 100.0 * reached_ms / game_ms;
    REVOSF2_CHECK(d.decay == 8000);
    REVOSF2_CHECK(loud_db > 0.1 && std::abs(loud_db - quiet_db) < 0.01);
}

void TestSfzDecayTakesTheGamesTime()
{
    // sfizz falls by e^9 (78.17 dB) over its decay time. The game falls 100 dB in 1000 / rate milliseconds. It falls
    // 78.17 dB in 78.17 / 100 of that time.
    const double decay_ms = 1000.0 / DecayRate(100);

    const SfzEnvelope d = ConvertEnvelopeForSfz(EnvelopeValues{127, 100, 64, 0, 100});

    const double kSfizzFallDb = 9.0 * 20.0 / std::log(10.0);
    REVOSF2_CHECK(std::abs(d.decay * 1000.0 - decay_ms * kSfizzFallDb / 100.0) < 1e-9 * decay_ms);
    REVOSF2_CHECK(d.release == d.decay && d.attack == 0.0 && d.hold == 0.0);
    REVOSF2_CHECK(std::abs(d.sustain - 100.0 * std::pow(10.0, SustainCentibels(64) / 200.0)) < 1e-9);

    // An attack of 127 is instant.
    const SfzEnvelope organ = ConvertEnvelopeForSfz(EnvelopeValues{127, 127, 127, 0, 127});
    REVOSF2_CHECK(organ.attack == 0.0 && organ.hold == 0.0 && organ.sustain == 100.0 && organ.release < 0.001);
}

void TestEnvelopeSimulationSustainsAndReleases()
{
    // Reach the sustain, then release until silent. Before its first update, a note releases from -90.4 dB, even with
    // an instant attack.
    REVOSF2_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 10) == SustainCentibels(64));
    REVOSF2_CHECK(HeldLevel(EnvelopeValues{127, 127, 64, 0, 127}, 0) == -904.0f);
    REVOSF2_CHECK(ReleaseFrames(0.0f, DecayRate(127)) == 1);
    REVOSF2_CHECK(ReleaseFrames(-500.0f, 1.0) == 135); // 134 steps of 3 end at -902, and the 135th is silent
    REVOSF2_CHECK(FramesToLevel(EnvelopeValues{127, 127, 127, 0, 127}, -400.0f) == -1);
    REVOSF2_CHECK(FramesToLevel(EnvelopeValues{127, 100, 0, 0, 127}, -400.0f) > 0);

    // Release 114 falls exactly 3 tenths of a dB a frame. From sustain 2 (-72.1 dB), the 61st frame reaches -90.4 dB.
    // That level is already silent.
    const float kSustain2 = static_cast<float>(SustainCentibels(2));
    REVOSF2_CHECK(ReleaseFrames(kSustain2, DecayRate(114)) == 61 && !Silent(-903.9f) && Silent(-904.0f));
}

void TestNoteIsQuietOnlyAfterItsAttack()
{
    // A slow attack starts below -40 dB, but hasn't fallen to it.
    const EnvelopeValues kSlowAttack{60, 100, 0, 0, 127};

    const int64_t frames = FramesToLevel(kSlowAttack, -400.0f);

    REVOSF2_CHECK(frames * kEnvStepMs > AttackMs(kSlowAttack.attack));
}

void TestPitchRatioIsTheGames()
{
    // Octaves are exact; the rest comes from the game's single-precision tables of 2^(i/12) and 2^(i/3072).
    REVOSF2_CHECK(PitchRatio(0) == 1.0f && PitchRatio(3072) == 2.0f && PitchRatio(-3072) == 0.5f);
    REVOSF2_CHECK(PitchRatio(256) == 0x1.0f38fap+0f); // a semitone
    int wrong_octave = 0, inexact = 0;
    for (int p = -3 * 3072; p <= 3 * 3072; p++)
    {
        wrong_octave += PitchRatio(p + 3072) != 2.0f * PitchRatio(p);
        inexact += std::abs(PitchRatio(p) / std::exp2(p / 3072.0) - 1.0) > 2e-7;
    }
    REVOSF2_CHECK(wrong_octave == 0 && inexact == 0);
}

void TestLfoWaitsThenTurns()
{
    // Depth 128, range 1 semitone, speed 64 (25 Hz) and a delay of 10 ms. It stays at 0 through the delay, then turns
    // an eighth of a cycle each 5 ms through the sine's four quarters.
    Lfo lfo;
    lfo.Set(128, 64, 1, 10);
    std::vector<float> values = {lfo.Value()};
    for (int i = 0; i < 8; i++)
    {
        lfo.Update(5);
        values.push_back(lfo.Value());
    }

    const float kEighth = 90.0f / 127.0f; // the table's sin(pi / 4)
    const std::vector<float> kExpected = {0, 0, 0, kEighth, 1, kEighth, 0, -kEighth, -1};
    REVOSF2_CHECK(values.size() == kExpected.size());
    for (std::size_t i = 0; i < values.size() && i < kExpected.size(); i++)
    {
        REVOSF2_CHECK(std::abs(values[i] - kExpected[i]) < 1e-6f);
    }

    Lfo off;
    off.Set(0, 64, 1, 0);
    off.Update(10);
    REVOSF2_CHECK(off.Value() == 0.0f);
}

void TestTrackFilterFollowsTheGame()
{
    // Low-pass arguments of 0 and below give the lowest cutoff. Arguments of 58 and above reach 16 kHz and turn AX's
    // filter off. AX's cutoffs are 32000 * 2^(10 (argument - 64) / 64) Hz.
    const std::pair<int, int> kIndex[] = {{-5, 0}, {0, 0}, {30, 30}, {57, 57}, {58, -1}, {0x40, -1}, {255, -1}};
    for (const auto& [arg, index] : kIndex)
    {
        REVOSF2_CHECK(LowPassIndex(arg) == index);
    }

    int off_formula = 0;
    for (int i = 0; i <= 57; i++)
    {
        off_formula += LowPassHz(i) != static_cast<int>(32000.0 * std::exp2(10.0 * (i - 64) / 64.0));
    }
    REVOSF2_CHECK(off_formula == 0 && LowPassHz(30) == 805);
}

void TestSoundFontFilterStandsInForTheGames()
{
    // No filter leaves the SoundFont's open; lower settings give lower cutoffs, without resonance.
    REVOSF2_CHECK(ToSoundFont(0x40, 0, 0) == SoundFontFilter{} && ToSoundFont(58, 0, 0) == SoundFontFilter{});
    REVOSF2_CHECK(ToSoundFont(30, 0, 0).cutoff < kFilterOpen && ToSoundFont(30, 0, 0).resonance == 0);
    int rising = 0;
    for (int arg = 1; arg < 58; arg++)
    {
        rising += ToSoundFont(arg, 0, 0).cutoff <= ToSoundFont(arg - 1, 0, 0).cutoff;
    }
    REVOSF2_CHECK(rising == 0);

    // Each controller step lowers the cutoff by 75 cents.
    REVOSF2_CHECK(FilterControllerValue(kFilterOpen) == 0 && FilterControllerValue(kFilterOpen - 75) == 1);
    REVOSF2_CHECK(FilterControllerValue(kFilterOpen - 75 * 111) == 111 && FilterControllerValue(0) == 127);
}

void TestSfzFilterFollowsTheGame()
{
    // The SFZ controllers: 64 minus the low-pass argument, and the biquad value (0 without a preset). At 6 or less, the
    // low-pass is off.
    REVOSF2_CHECK(SfzLowPassValue(0x40) == 0 && SfzLowPassValue(200) == 0 && SfzLowPassValue(58) == 6);
    REVOSF2_CHECK(SfzLowPassValue(57) == 7 && SfzLowPassValue(0) == 64 && SfzLowPassValue(-100) == 127);
    REVOSF2_CHECK(SfzBiquadValue(1, 64) == 64 && SfzBiquadValue(2, 300) == 127 && SfzBiquadValue(0, 64) == 0);
    REVOSF2_CHECK(SfzBiquadValue(6, 64) == 0 && SfzBiquadValue(3, -5) == 0);

    // A biquad type with a preset and a positive value select a row. Value 64 selects the low-pass's row 55, and 127 or
    // more each table's last. The band-passes scale value * (2 - value).
    REVOSF2_CHECK(BiquadStep(1, 64) == 55 && BiquadStep(1, 255) == 111 && BiquadStep(2, 127) == 96);
    REVOSF2_CHECK(BiquadStep(3, 127) == 121 && BiquadStep(5, 127) == 92 && BiquadStep(4, 64) == 69);
    REVOSF2_CHECK(BiquadStep(1, 0) == -1 && BiquadStep(0, 64) == -1 && BiquadStep(6, 64) == -1);

    // AX's pole for 31 Hz is sfizz's one-pole at 31.5 Hz, 11190 cents below 20 kHz, and lower settings still give lower
    // cutoffs.
    REVOSF2_CHECK(SfzLowPassCutoff(0) == -11190);
    int falling = 0;
    for (int i = 1; i <= 57; i++)
    {
        falling += SfzLowPassCutoff(i) <= SfzLowPassCutoff(i - 1);
    }
    REVOSF2_CHECK(falling == 0);
}

void TestSoundFontPanGivesTheGamesGains()
{
    // The converted pan makes the sin/cos law give NW4R SND's square-root law.
    for (int i = -19; i <= 19; i++)
    {
        const double p = i / 20.0;
        const double w = SoundFontPan(p);

        const double r_sf = std::sin((w + 1.0) * std::numbers::pi / 4.0);
        const double l_sf = std::cos((w + 1.0) * std::numbers::pi / 4.0);

        REVOSF2_CHECK(std::abs(r_sf - std::sqrt((1.0 + p) / 2.0)) < 1e-9);
        REVOSF2_CHECK(std::abs(l_sf - std::sqrt((1.0 - p) / 2.0)) < 1e-9);
    }

    REVOSF2_CHECK(SoundFontPan(0.0) == 0.0 && SoundFontPan(1.0) == 1.0 && SoundFontPan(-1.0) == -1.0);
}

void TestStereoWavesKeepTheirSidesInBalanceMode()
{
    // In balance mode a stereo wave's channels stay on their sides, at the level the pan law gives the side; in dual
    // mode each pans from its side at full level. A mono wave plays at the pan in both.
    const ChannelPan centre = WaveChannelPan(0.0, 0, 2, true);
    const ChannelPan right = WaveChannelPan(0.5, 1, 2, true);
    const ChannelPan dual = WaveChannelPan(0.5, 0, 2, false);
    const ChannelPan mono = WaveChannelPan(0.3, 0, 1, true);

    REVOSF2_CHECK(centre.pan == -1.0 && std::abs(centre.gain_db - 10.0 * std::log10(0.5)) < 1e-9);
    REVOSF2_CHECK(right.pan == 1.0 && std::abs(right.gain_db - 10.0 * std::log10(0.75)) < 1e-9);
    REVOSF2_CHECK(dual.pan == -0.5 && dual.gain_db == 0.0 && mono.pan == 0.3 && mono.gain_db == 0.0);
}

void TestMissingBlockIsReported()
{
    // An RWAV whose header has an INFO block but no DATA block.
    Writer w = FileHeader("RWAV", 0x0102, 2);
    const std::size_t info = StartBlock(w, "INFO");
    w.Add(WaveInfoSize(1));
    Block(w, 0, info);

    std::string message;
    try
    {
        Wave::ParseFile(w.bytes);
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    REVOSF2_CHECK(message == "no INFO or DATA block");
}

void TestShortFileIsReported()
{
    std::string message;
    try
    {
        Wave::ParseFile(std::vector<uint8_t>{'R', 'W', 'A', 'V'});
    }
    catch (const FormatError& e)
    {
        message = e.what();
    }

    REVOSF2_CHECK(message == "too short to be an RWAV file");
}

// Play the first `n` samples of `data`, and loop over [ls, le).
std::vector<int16_t> Play(const std::vector<int16_t>& data, uint32_t ls, uint32_t le, std::size_t n)
{
    std::vector<int16_t> out;
    for (std::size_t i = 0; out.size() < n; i++)
    {
        out.push_back(data[i < le ? i : ls + (i - le) % (le - ls)]);
    }

    return out;
}

void TestLoopUnrollingKeepsTheStream()
{
    // Unroll a four-sample loop starting at 2: its start moves to 8 or later, it grows to 32 samples or more, and data
    // from its start follows its end. It plays the same.
    Pcm pcm;
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 6;
    pcm.channels = {{10, 11, 20, 21, 22, 23}};

    const PreparedWave w = PrepareForSoundFont(pcm);

    const auto& s = w.channels[0];
    REVOSF2_CHECK(w.loop && w.loop_start >= 8 && w.loop_end - w.loop_start >= 32);
    REVOSF2_CHECK((w.loop_end - w.loop_start) % 4 == 0);
    REVOSF2_CHECK(s.size() >= w.loop_end + 8);
    REVOSF2_CHECK(Play(s, w.loop_start, w.loop_end, 200) == Play(pcm.channels[0], 2, 6, 200));
    for (uint32_t i = 0; i < 8; i++)
    {
        REVOSF2_CHECK(s[w.loop_end + i] == s[w.loop_start + i]);
    }
}

void TestSfzLoopUnrollingKeepsTheStream()
{
    // At 32000 Hz, 4 ms is 128 samples. A four-sample loop starting at 2 gets 32 copies of itself (128 samples) before
    // it, 16 within it (64 samples) and 64 samples after it. It plays the same. The data before the loop's two ends is
    // the same. sfizz's crossfade is then inaudible.
    Pcm pcm;
    pcm.sample_rate = 32000;
    pcm.loop = true;
    pcm.loop_start = 2;
    pcm.loop_end = 6;
    pcm.channels = {{10, 11, 20, 21, 22, 23}};

    const PreparedWave w = PrepareForSfz(pcm);

    const auto& s = w.channels[0];
    REVOSF2_CHECK(w.loop && w.loop_start == 130 && w.loop_end == 194 && s.size() == 258);
    REVOSF2_CHECK(Play(s, w.loop_start, w.loop_end, 400) == Play(pcm.channels[0], 2, 6, 400));
    int different = 0;
    for (uint32_t i = 1; i <= 128; i++)
    {
        different += s[w.loop_start - i] != s[w.loop_end - i];
    }
    REVOSF2_CHECK(different == 0);
}

void TestOneShotWaveStaysAsItIs()
{
    Pcm once;
    once.loop_end = 3;
    once.channels = {{1, 2, 3}, {4, 5, 6}};

    const PreparedWave o = PrepareForSoundFont(once);

    REVOSF2_CHECK(!o.loop && o.channels.size() == 2 && o.channels[1] == std::vector<int16_t>({4, 5, 6}));
}

void TestSoundFontWriterOutputIsValid()
{
    sf2::SoundFont sf;
    sf.name = "test";
    for (int c = 0; c < 3; c++)
    {
        sf2::Sample s;
        s.name = "s" + std::to_string(c);
        s.data.assign(100, static_cast<int16_t>(c * 1000));
        s.loop_start = 10;
        s.loop_end = 90;
        sf.samples.push_back(s);
    }

    sf.samples[1].type = sf2::kLeft;
    sf.samples[1].link = 2;
    sf.samples[2].type = sf2::kRight;
    sf.samples[2].link = 1;

    sf2::Instrument inst;
    inst.name = "i";
    sf2::Zone global;
    global.modulators.push_back({0x0081, sf2::kVibLfoToPitch, 100, 0});
    inst.zones.push_back(global);
    for (uint16_t id = 0; id < 3; id++)
    {
        sf2::Zone z;
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleId, id)); // written last whatever the order
        z.generators.push_back(sf2::Generator::Value(sf2::kSampleModes, 1));
        z.generators.push_back(sf2::Generator::Range(sf2::kVelRange, 0, 127));
        z.generators.push_back(sf2::Generator::Range(sf2::kKeyRange, 0, 127));
        inst.zones.push_back(z);
    }
    sf.instruments.push_back(inst);

    sf2::Preset p;
    p.name = "p";
    sf2::Zone pz;
    pz.generators.push_back(sf2::Generator::Value(sf2::kInstrument, 0));
    p.zones.push_back(pz);
    sf.presets.push_back(p);

    const auto bytes = sf2::Write(sf);

    const std::string problem = Sf2Problem(bytes);
    REVOSF2_CHECK(problem.empty());
    if (!problem.empty())
    {
        std::fprintf(stderr, "  SoundFont writer: %s\n", problem.c_str());
    }

    const Sf2View v = ParseSf2(bytes);
    REVOSF2_CHECK(v.chunks.at("phdr").second == 38 * 2 && v.chunks.at("inst").second == 22 * 2);
    REVOSF2_CHECK(v.chunks.at("smpl").second == 2 * 3 * (100 + 46));
    REVOSF2_CHECK(v.chunks.at("imod").second == 10 * 2);
}

void TestSoundFontWithoutSamplesHasSampleData()
{
    // A silent sequence still gets sample padding: FluidSynth rejects empty sample data.
    const auto bytes = sf2::Write(sf2::SoundFont{});

    const Sf2View v = ParseSf2(bytes);

    REVOSF2_CHECK(Sf2Problem(bytes).empty() && v.chunks.at("smpl").second == 2 * 46);
}

void TestBalanceModeKeepsStereoWavesAt3DbDown()
{
    // In balance mode, a stereo wave at a pan of 0 plays each channel 3 dB down. The SoundFont's samples are 3 dB down
    // too. In dual mode, the wave plays each channel at full level.
    std::vector<ArchiveFile> files(2);
    files[0].data = Rseq({0x3c, 0x64, 0x30, 0xff});
    files[1].data = Rbnk({TestInstrument{}});
    files[1].wave_data = Rwar({Rwav(64, std::vector<int16_t>(64, 1000), 2, true)});
    const SoundArchive archive =
        SoundArchive::Load(Rsar(files, {.sequences = {{.pan_mode = 1}, {.pan_mode = 0}}, .banks = {1}}));

    const Conversion balance = ConvertSequence(archive, 0);
    const Conversion dual = ConvertSequence(archive, 1);

    auto first_sample = [](const std::vector<uint8_t>& sf2)
    {
        const Sf2View v = ParseSf2(sf2);
        return static_cast<int16_t>(Le16(sf2, v.chunks.at("smpl").first));
    };
    REVOSF2_CHECK(first_sample(balance.sf2) == 707 && first_sample(dual.sf2) == 1000);
}

void TestMidiWriterLayersMarkersAndBendRange()
{
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.loop = Performance::Loop{48, 96};
    p.end_tick = 96;
    p.presets.push_back({});

    auto add = [&](int t, uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        p.tracks[t].push_back(e);
    };

    add(0, 0, EventKind::kPreset, 0, 0, 0);
    add(0, 0, EventKind::kVolume, 0, 100, 0);
    add(0, 0, EventKind::kNoteOn, 60, 100, 1);
    add(0, 10, EventKind::kNoteOn, 60, 90, 2); // the same key again: another channel
    add(0, 20, EventKind::kNoteOff, 60, 0, 1);
    add(0, 30, EventKind::kNoteOff, 60, 0, 2);
    add(3, 5, EventKind::kNoteOn, 64, 80, 3);
    add(3, 50, EventKind::kNoteOff, 64, 0, 3);
    TrackEvent bend;
    bend.tick = 5;
    bend.kind = EventKind::kPitch;
    bend.semitones = -7.5f;
    p.tracks[3].insert(p.tracks[3].begin(), bend);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    // Timebase 48 gives 160 MIDI ticks for each of the performance's ticks.
    const MidiView v = ParseMidi(bytes);
    REVOSF2_CHECK(v.ok);
    REVOSF2_CHECK(v.division == 7680);
    REVOSF2_CHECK(rep.channels == 3 && rep.cut_notes == 0);
    REVOSF2_CHECK(v.track_names.size() == 4 && v.track_names[0] == "test" && v.track_names[2] == "Track 0 (layer 2)");
    REVOSF2_CHECK(v.notes.size() == 3);

    // Track 0 gets channel 0, and channel 2 for its layer (track 3 has channel 1).
    int channels_for_key60 = 0;
    for (const MidiNote& n : v.notes)
    {
        if (n.key == 60)
        {
            channels_for_key60 |= 1 << n.channel;
        }
    }
    REVOSF2_CHECK(channels_for_key60 == 0b101);

    REVOSF2_CHECK(v.markers.size() == 2 && v.markers[0].first == 7680 && v.markers[1].second == "loopEnd");

    // Pitch bend range 8 (RPN 0) for a 7.5 semitone bend on track 3's channel (1).
    const std::vector<uint8_t> kRpn = {0xb1, 6, 8};
    REVOSF2_CHECK(std::search(bytes.begin(), bytes.end(), kRpn.begin(), kRpn.end()) != bytes.end());
}

void TestMidiWriterGivesPartsChannelsOfTheirOwn()
{
    // Track 0 plays key 60 on its first part and key 62 on its second part. Only the second part sets volume 50.
    Performance p;
    p.timebase = 48;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});

    auto add = [&](uint32_t tick, EventKind k, uint8_t key, uint8_t value, uint32_t note, uint8_t part)
    {
        TrackEvent e;
        e.tick = tick;
        e.kind = k;
        e.key = key;
        e.value = value;
        e.note = note;
        e.part = part;
        p.tracks[0].push_back(e);
    };
    add(0, EventKind::kPreset, 0, 0, 0, 0);
    add(0, EventKind::kNoteOn, 60, 100, 1, 0);
    add(10, EventKind::kPreset, 0, 0, 0, 1);
    add(10, EventKind::kVolume, 0, 50, 0, 1);
    add(10, EventKind::kNoteOn, 62, 100, 2, 1);
    add(20, EventKind::kNoteOff, 60, 0, 1, 0);
    add(30, EventKind::kNoteOff, 62, 0, 2, 1);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    const MidiView v = ParseMidi(bytes);
    const std::vector<uint8_t> kSecondVolume = {0xb1, 7, 50}, kFirstVolume = {0xb0, 7, 50};
    REVOSF2_CHECK(v.ok && rep.channels == 2 && v.track_names.size() == 3 && v.track_names[2] == "Track 0 (layer 2)");
    REVOSF2_CHECK(v.notes.size() == 2 && v.notes[0].channel != v.notes[1].channel);
    REVOSF2_CHECK(std::search(bytes.begin(), bytes.end(), kSecondVolume.begin(), kSecondVolume.end()) != bytes.end() &&
                  std::search(bytes.begin(), bytes.end(), kFirstVolume.begin(), kFirstVolume.end()) == bytes.end());
}

void TestMidiWriterHasAtLeast7680TicksPerQuarterNote()
{
    // At least 7680 MIDI ticks per quarter note, a whole number of them to each of the performance's ticks. Without
    // frame times, notes stay where they are.
    const std::pair<uint32_t, uint16_t> kCases[] = {{48, 7680}, {96, 7680}, {100, 7700}, {255, 7905}, {1, 7680}};
    for (const auto& [timebase, division] : kCases)
    {
        Performance p;
        p.timebase = timebase;
        p.tempo = {{0, 120.0}};
        p.end_tick = 8;
        p.presets.push_back({});
        TrackEvent on;
        on.tick = 3;
        on.key = 60;
        on.value = 100;
        on.note = 1;
        TrackEvent off = on;
        off.tick = 7;
        off.kind = EventKind::kNoteOff;
        p.tracks[0] = {on, off};

        MidiReport rep;
        const MidiView v = ParseMidi(WriteMidi(p, "test", rep));
        const uint32_t scale = division / timebase;
        REVOSF2_CHECK(v.ok && v.division == division);
        REVOSF2_CHECK(v.notes.size() == 1 && v.notes[0].on == 3 * scale && v.notes[0].off == 7 * scale);
    }
}

void TestMidiEventsGoOnSoundFrames()
{
    // At timebase 48, 160 MIDI ticks to a tick. Events go at the starts of their frames, given as fractions of ticks,
    // without crossing the loop start at tick 2. Keys 60, 62 and 64 play over ticks 1-2, 2-3 and 3-4, and a sweep bends
    // key 64 up a semitone in a frame between ticks 3 and 4, half a tick after tick 3.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 8;
    p.presets.push_back({});
    p.frame_ticks = {0.0, 0.5, 1.75, 2.75, 4.0};
    p.loop = Performance::Loop{2, 6};
    const std::pair<uint8_t, uint32_t> kNotes[] = {{60, 1}, {62, 2}, {64, 3}}; // key, tick
    uint32_t id = 1;
    for (const auto& [key, tick] : kNotes)
    {
        TrackEvent on;
        on.tick = tick;
        on.key = key;
        on.value = 100;
        on.note = id++;
        TrackEvent off = on;
        off.tick = tick + 1;
        off.kind = EventKind::kNoteOff;
        p.tracks[0].push_back(on);
        p.tracks[0].push_back(off);
    }
    TrackEvent bend;
    bend.tick = 3;
    bend.kind = EventKind::kPitch;
    bend.semitones = 1.0f;
    bend.after = 0.5;
    p.tracks[0].insert(p.tracks[0].end() - 1, bend);

    MidiReport rep;
    const MidiView m = ParseMidi(WriteMidi(p, "test", rep));

    REVOSF2_CHECK(m.ok && m.notes.size() == 3);
    REVOSF2_CHECK(m.bends == (std::vector<std::pair<uint32_t, int>>{{560, 12288}}));
    if (m.notes.size() == 3)
    {
        REVOSF2_CHECK(m.notes[0].key == 60 && m.notes[0].on == 80 && m.notes[0].off == 320);
        REVOSF2_CHECK(m.notes[1].key == 62 && m.notes[1].on == 320 && m.notes[1].off == 440);
        REVOSF2_CHECK(m.notes[2].key == 64 && m.notes[2].on == 440 && m.notes[2].off == 640);
    }
    REVOSF2_CHECK(m.markers == (std::vector<std::pair<uint32_t, std::string>>{{320, "loopStart"}, {960, "loopEnd"}}));
}

void TestMidiWriterSplitsLongDeltaTimes()
{
    // At timebase 1, each tick is 7680 MIDI ticks. A note of 600000 ticks goes past both 32-bit MIDI positions and the
    // 28-bit limit of a delta time.
    Performance p;
    p.timebase = 1;
    p.tempo = {{0, 120.0}};
    p.end_tick = 600000;
    p.presets.push_back({});
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent off = on;
    off.tick = 600000;
    off.kind = EventKind::kNoteOff;
    p.tracks[0] = {on, off};

    MidiReport rep;
    const MidiView v = ParseMidi(WriteMidi(p, "test", rep));

    REVOSF2_CHECK(v.ok && v.track_ends == std::vector<uint64_t>(2, uint64_t{600000} * 7680));
}

void TestMidiBendUpByAWholeRangeIsExact()
{
    // A bend of +7 semitones requires range 8. The pitch wheel stops at +8191, a step short of the whole range. With
    // range 8, +7 is 7168 steps: wheel value 15360 (0x3c00, bytes 0 and 0x78).
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent bend;
    bend.kind = EventKind::kPitch;
    bend.semitones = 7.0f;
    p.tracks[0].push_back(bend);
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    p.tracks[0].push_back(on);
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    p.tracks[0].push_back(off);

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    const std::vector<uint8_t> kRange = {0xb0, 6, 8};
    const std::vector<uint8_t> kBend = {0xe0, 0x00, 0x78};
    REVOSF2_CHECK(std::search(bytes.begin(), bytes.end(), kRange.begin(), kRange.end()) != bytes.end());
    REVOSF2_CHECK(std::search(bytes.begin(), bytes.end(), kBend.begin(), kBend.end()) != bytes.end());
}

void TestMidiWriterSendsTheFilterToEveryLayer()
{
    // Overlapping strikes of one key need a second channel, and both get the filter's controller.
    Performance p;
    p.tempo = {{0, 120.0}};
    p.end_tick = 48;
    p.presets.push_back({});
    TrackEvent filter;
    filter.kind = EventKind::kFilter;
    filter.value = 40;
    TrackEvent on;
    on.key = 60;
    on.value = 100;
    on.note = 1;
    TrackEvent again = on;
    again.tick = 12;
    again.note = 2;
    TrackEvent off = on;
    off.tick = 24;
    off.kind = EventKind::kNoteOff;
    TrackEvent off_again = again;
    off_again.tick = 36;
    off_again.kind = EventKind::kNoteOff;
    p.tracks[0] = {filter, on, again, off, off_again};

    MidiReport rep;
    const auto bytes = WriteMidi(p, "test", rep);

    REVOSF2_CHECK(rep.channels == 2);
    for (const int channel : {0, 1})
    {
        const std::vector<uint8_t> kFilter = {static_cast<uint8_t>(0xb0 | channel), kFilterController, 40};
        REVOSF2_CHECK(std::search(bytes.begin(), bytes.end(), kFilter.begin(), kFilter.end()) != bytes.end());
    }
}

void TestMidiWriterListsChannel10Presets()
{
    // Track t uses preset t. With 16 tracks the last gets channel 10 (index 9), whose preset has to be copied to bank
    // 128; with 15, channel 10 isn't used.
    for (int tracks : {15, 16})
    {
        Performance p;
        p.tempo = {{0, 120.0}};
        p.end_tick = 48;
        for (int t = 0; t < tracks; t++)
        {
            TrackEvent preset;
            preset.kind = EventKind::kPreset;
            preset.preset = static_cast<uint32_t>(t);
            TrackEvent on;
            on.key = 60;
            on.value = 100;
            on.note = static_cast<uint32_t>(t + 1);
            TrackEvent off = on;
            off.tick = 24;
            off.kind = EventKind::kNoteOff;
            p.presets.push_back({});
            p.tracks[t] = {preset, on, off};
        }

        MidiReport rep;
        WriteMidi(p, "test", rep);

        REVOSF2_CHECK(rep.channels == tracks);
        REVOSF2_CHECK(rep.channel_10_presets == (tracks == 16 ? std::set<uint32_t>{15} : std::set<uint32_t>{}));
    }
}

void TestWavFileIsMonoPcm16()
{
    const std::vector<uint8_t> wav = WavFile(std::vector<int16_t>{1, -2, 32767}, 32000);

    REVOSF2_CHECK(wav.size() == 50 && Text(std::span(wav).first(4)) == "RIFF" && Le32(wav, 4) == 42);
    REVOSF2_CHECK(Text(std::span(wav).subspan(8, 8)) == "WAVEfmt " && Le32(wav, 16) == 16);
    // PCM, one channel, 32000 Hz, 2 bytes a frame, 16 bits.
    REVOSF2_CHECK(Le16(wav, 20) == 1 && Le16(wav, 22) == 1 && Le32(wav, 24) == 32000 && Le32(wav, 28) == 64000);
    REVOSF2_CHECK(Le16(wav, 32) == 2 && Le16(wav, 34) == 16);
    REVOSF2_CHECK(Text(std::span(wav).subspan(36, 4)) == "data" && Le32(wav, 40) == 6);
    REVOSF2_CHECK(Le16(wav, 44) == 1 && Le16(wav, 46) == 0xfffe && Le16(wav, 48) == 32767);
}

void TestSfzFileHasTheTracksPresetsAndFilter()
{
    // Track 2 plays presets 0 and 1 with low-pass argument 4 (48 Hz) and the biquad high-pass (type 2). The other
    // tracks play nothing, and get no MIDI channel or SFZ file. With no banks in the archive, the presets have no
    // regions.
    const SoundArchive archive = SoundArchive::Load(Rsar({}));
    BankSet banks(archive, SoundInfo{});
    Performance p;
    p.presets.resize(2);
    TrackEvent low_pass;
    low_pass.kind = EventKind::kSfzLowPass;
    low_pass.value = SfzLowPassValue(4);
    TrackEvent biquad;
    biquad.kind = EventKind::kSfzBiquad;
    biquad.value = SfzBiquadValue(2, 64);
    TrackEvent first;
    first.kind = EventKind::kPreset;
    TrackEvent second = first;
    second.preset = 1;
    p.tracks[2] = {low_pass, biquad, first, second};
    p.biquad_types[2] = 2;
    p.level_cut_db = 2.5;
    MidiReport midi;
    midi.bend_ranges[2] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "TITLE", {"first", "second"}, true);

    REVOSF2_CHECK(sfz.files.size() == 1 && sfz.samples.empty());
    if (sfz.files.size() != 1)
    {
        return;
    }

    const SfzFile& f = sfz.files[0];
    auto has = [&](const std::string& s)
    {
        return f.text.find(s) != std::string::npos;
    };
    REVOSF2_CHECK(f.name == "Track 2.sfz");
    REVOSF2_CHECK(has("MIDI gain was reduced by 2.5 dB. Region volumes restore that gain.\n"));
    REVOSF2_CHECK(has("<control>\ndefault_path=../samples/\n"));
    REVOSF2_CHECK(has("bend_up=199.975586 bend_down=-199.975586\n"));

    // CC7, CC10 and CC11 act at once, with CC7 and CC11 squared.
    REVOSF2_CHECK(has("pan_oncc10=100 pan_curvecc10=7 pan_smoothcc10=0\n"));
    REVOSF2_CHECK(
        has("amplitude_oncc7=100 amplitude_curvecc7=4 amplitude_smoothcc7=0 amplitude_oncc11=100 "
            "amplitude_curvecc11=4 amplitude_smoothcc11=0\n"));

    // The low-pass is filter 1. Controller value 60 gives sfizz's cutoff for AX's at 48 Hz, 10440 cents below 20 kHz,
    // and 0 leaves the filter open.
    REVOSF2_CHECK(low_pass.value == 60);
    REVOSF2_CHECK(has("fil_type=lpf_1p cutoff=20000 cutoff_oncc103=12000 cutoff_curvecc103=8\n"));
    REVOSF2_CHECK(has("<curve> curve_index=8\nv000=0 ") && has(" v060=-0.87 "));

    // Each preset gets a master. Notes starting with the biquad controller above 0 use the high-pass in filter slot 2.
    REVOSF2_CHECK(has("// first\n<master> loprog=0 hiprog=0\n") && has("// second\n<master> loprog=1 hiprog=1\n"));
    REVOSF2_CHECK(has("<group> hicc104=0\n<group> locc104=1 fil2_type=hpf_2p cutoff2=1000 "));
}

void TestSfzCommentKeepsTheNameOnItsLine()
{
    // A line break in a sequence's name could end its comment and start SFZ opcodes. Without banks, any region would
    // have come from the name.
    const SoundArchive archive = SoundArchive::Load(Rsar({}));
    BankSet banks(archive, SoundInfo{});
    Performance p;
    p.presets.resize(1);
    TrackEvent preset;
    preset.kind = EventKind::kPreset;
    p.tracks[0] = {preset};
    MidiReport midi;
    midi.bend_ranges[0] = 2;

    const SfzInstruments sfz = BuildSfz(p, midi, banks, "A\r\n<region> sample=x.wav", {"b0 p0"}, true);

    REVOSF2_CHECK(sfz.files.size() == 1);
    for (const SfzFile& f : sfz.files)
    {
        REVOSF2_CHECK(f.text.starts_with("// Track 0 of A  <region> sample=x.wav, converted by revosf2"));
        REVOSF2_CHECK(f.text.find("\n<region>") == std::string::npos);
    }
}

void TestFileNameKeepsValidNames()
{
    REVOSF2_CHECK(FileName(".x. y") == ".x. y");
}

void TestFileNameReplacesCharactersWindowsForbids()
{
    // Path separators included.
    REVOSF2_CHECK(FileName("a<b>c:d\"e/f\\g|h?i*j") == "a_b_c_d_e_f_g_h_i_j");
    REVOSF2_CHECK(FileName("../x") == ".._x");
    REVOSF2_CHECK(FileName(std::string("a\tb\0c", 5)) == "a_b_c");

    // No trailing dots or spaces in directory names, "." and ".." included.
    REVOSF2_CHECK(FileName("BGM.") == "BGM_" && FileName("BGM ") == "BGM_" && FileName("CON.") == "CON_");
    REVOSF2_CHECK(FileName(".") == "_" && FileName("..") == "._");
}

void TestFileNameAvoidsDeviceNames()
{
    // '_' goes before the first dot.
    REVOSF2_CHECK(FileName("NUL") == "NUL_");
    REVOSF2_CHECK(FileName("con.x") == "con_.x");
    REVOSF2_CHECK(FileName("Com1") == "Com1_");
    REVOSF2_CHECK(FileName("nul .x") == "nul _.x");
    REVOSF2_CHECK(FileName("LPT¹") == "LPT¹_");
    REVOSF2_CHECK(FileName("CONOUT$") == "CONOUT$_");
    REVOSF2_CHECK(FileName("CONSOLE") == "CONSOLE");
}

// Check that the SFZ files match the tracks in `midi`, play the sequence's WAV samples and loop within them. Return the
// problem, or an empty string.
std::string SfzProblem(const SfzInstruments& sfz, const MidiView& midi)
{
    std::map<std::string, uint32_t> frames;
    for (const SfzSample& s : sfz.samples)
    {
        const std::vector<uint8_t>& w = s.wav;
        if (w.size() < 44 || Text(std::span(w).first(4)) != "RIFF" || Le32(w, 4) != w.size() - 8 ||
            Le32(w, 40) != w.size() - 44)
        {
            return "broken WAV file " + s.name;
        }

        frames[s.name] = Le32(w, 40) / 2;
    }

    std::set<std::string> names, tracks;
    for (const SfzFile& f : sfz.files)
    {
        names.insert(f.name);
    }
    for (std::size_t t = 1; t < midi.track_names.size(); t++) // skip the conductor track
    {
        const std::string& track = midi.track_names[t];
        tracks.insert(track.substr(0, track.find(" (layer")) + ".sfz");
    }
    if (names != tracks)
    {
        return std::to_string(names.size()) + " files for " + std::to_string(tracks.size()) + " tracks";
    }

    for (const SfzFile& f : sfz.files)
    {
        std::istringstream lines(f.text);
        for (std::string line; std::getline(lines, line);)
        {
            if (!line.starts_with("<region> "))
            {
                continue;
            }

            std::map<std::string, std::string> opcodes;
            std::istringstream words(line.substr(9));
            for (std::string word; words >> word;)
            {
                opcodes[word.substr(0, word.find('='))] = word.substr(word.find('=') + 1);
            }

            const auto it = frames.find(opcodes["sample"]);
            if (it == frames.end())
            {
                return f.name + " plays a sample the sequence doesn't have: " + opcodes["sample"];
            }
            if (opcodes.count("loop_end"))
            {
                const unsigned long start = std::stoul(opcodes["loop_start"]), end = std::stoul(opcodes["loop_end"]);
                if (start >= end || end >= it->second)
                {
                    return f.name + " loops outside " + it->first;
                }
            }
        }
    }

    return "";
}

void TestArchiveConvertsEverySequence(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path);
        failures++;
        return;
    }

    const SoundArchive archive = SoundArchive::Load(std::move(bytes));
    int sequences = 0, looping = 0;
    for (uint32_t i = 0; i < archive.Sounds().size(); i++)
    {
        const SoundInfo& s = archive.Sounds()[i];
        if (s.type != SoundType::kSequence || archive.FileMissing(s.file_id))
        {
            continue; // a sequence missing from a truncated archive can't be converted, and revosf2 says so
        }

        sequences++;
        Conversion c;
        try
        {
            c = ConvertSequence(archive, i);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", s.name.c_str(), e.what());
            failures++;
            continue;
        }

        const MidiView m = ParseMidi(c.midi);
        const std::string sf2_problem = Sf2Problem(c.sf2);
        const std::string sfz_problem = SfzProblem(c.sfz, m);
        if (!m.ok || !sf2_problem.empty() || !sfz_problem.empty())
        {
            std::fprintf(stderr, "%s: midi %s, sf2 %s, sfz %s\n", s.name.c_str(), m.ok ? "ok" : "broken",
                         sf2_problem.empty() ? "ok" : sf2_problem.c_str(),
                         sfz_problem.empty() ? "ok" : sfz_problem.c_str());
            failures++;
        }

        // Events of a tick go no later than the tick, and no earlier than the frame of the tick before.
        const std::vector<double>& frames = c.performance.frame_ticks;
        for (std::size_t k = 0; k < frames.size(); k++)
        {
            if (frames[k] > static_cast<double>(k) || (k > 0 && frames[k] < frames[k - 1]))
            {
                std::fprintf(stderr, "%s: tick %zu's sound frame starts at tick %g\n", s.name.c_str(), k, frames[k]);
                failures++;
                break;
            }
        }

        if (c.performance.loop)
        {
            looping++;
        }
    }

    std::printf("archive: %d sequences, %d loop\n", sequences, looping);
    REVOSF2_CHECK(sequences > 0);
}

} // namespace

int main(int argc, char** argv)
{
    TestDspAdpcmScalesNibbles();
    TestDspAdpcmPredictsFromHistory();
    TestDspAdpcmClampsLargePredictions();
    TestPcm16WaveDecodes();
    TestWaveLongerThanItsDataIsReported();
    TestWaveDecodesItsFirstTwoChannels();
    TestAdpcmLoopIsInSamples();
    TestMissingBlockIsReported();
    TestShortFileIsReported();
    TestVersionOneBankDescribesItsWaves();
    TestBankSelectsRegionsAsTheGameDoes();
    TestOldBankHasNoVolumeOrTune();
    TestFilesAreFoundInGroups();
    TestDamagedNameTableLeavesSoundsNumbered();
    TestTruncatedArchiveKeepsWhatItHolds();
    TestCopyWithWaveDataIsPreferred();
    TestTruncatedArchiveWhoseTablesCantBeReadIsReported();
    TestOtherPlatformsArchivesAreReported();
    TestNamesHaveNoControlCharacters();
    TestBrokenBankReferencesAreReported();
    TestWaveDataPastTheEndIsReported();
    TestSequenceStartingPastItsEndPlaysNothing();
    TestEnvelopeValuesMatchTheGame();
    TestRegionsHoldComesWith2008();
    TestInstantEnvelopeIsAnOrgan();
    TestDecayTakesTheGamesTime();
    TestSlowDecaySplitsItsError();
    TestSfzDecayTakesTheGamesTime();
    TestEnvelopeSimulationSustainsAndReleases();
    TestNoteIsQuietOnlyAfterItsAttack();
    TestPitchRatioIsTheGames();
    TestLfoWaitsThenTurns();
    TestTrackFilterFollowsTheGame();
    TestSoundFontFilterStandsInForTheGames();
    TestSfzFilterFollowsTheGame();
    TestSoundFontPanGivesTheGamesGains();
    TestStereoWavesKeepTheirSidesInBalanceMode();
    TestOnlyAJumpBackToPlayedCommandsIsALoop();
    TestMainLoopWaitsForAFade();
    TestLevelTooLoudForMidiIsCut();
    TestFullLevelStopsTheTrackVolumes();
    TestLegatoLeavesOutTheRegionsVolume();
    TestRisingLegatoCarriesItsNoteOn();
    TestRisingLegatoBesideATailTakesAnotherPart();
    TestDetachedTailKeepsItsSettings();
    TestSweepLeavesTheOlderNoteItsPitch();
    TestPartLeftTakesTheBendInItsTick();
    TestLegatoAfterAnEnvelopeResetCarriesOn();
    TestLegatoOnADecayedNoteStaysSilent();
    TestLegatoBelowHearingKeepsItsMidiNote();
    TestMidiNoteOfAnEndlessNoteEndsBelowHearing();
    TestHeldNoteBelowHearingEndsTheSequence();
    TestSilentLegatoIsNotReported();
    TestSilentNoteLeavesTheTailItsPitch();
    TestTieGivesTheTailTheTicksVolume();
    TestNoteAboveVelocity127TakesItsLevel();
    TestMuteStopLeavesDetachedTails();
    TestRisingLegatoKeepsToItsRegion();
    TestLevelCutFitsTheRunThatsKept();
    TestTiedNoteTakesPartInTheMainLoop();
    TestSilentLegatoCarriesItsNoteOn();
    TestLegatosChangeOfKeyIsNoChangeOfTheTracks();
    TestMonoLegatoTakesItsLengthWithTieOn();
    TestNoteReleasedBeforeItsFirstFrameIsSilent();
    TestWhatMidiCantSeparateIsReported();
    TestOneShotWaveEndsAsTheDspPlaysIt();
    TestTicksFallInTheGamesSoundFrames();
    TestOddArgumentsAreReadAsTheGameReadsThem();
    TestSequenceThatStopsAdvancingPlaysOut();
    TestTieLoopThatHoldsEndsAsAHeldSound();
    TestTieLoopOverAOneShotWaveStaysALoop();
    TestClosingTickReachesTheReleaseTail();
    TestSilenceAtTheTimeLimitEndsWithTheLastSound();
    TestOneShotWavePlaysOutAtItsPitch();
    TestTimedSweepGlidesOnWhenTheSequenceStops();
    TestTimedSweepMovesEveryFrame();
    TestTimedSweepGoesOnAfterItsTrackCloses();
    TestIdleTrackDoesNotOwnTheLoopWhileAnotherPlays();
    TestTrackSettingAVariableAnotherReadsOwnsTheLoop();
    TestVariableSetAfterAJumpBackBelongsToTheNewPass();
    TestTrackWaitingInSilenceDoesNotHoldOffTheLoop();
    TestPollAfterTheSoundIsNoLoop();
    TestSilentPassesThatChangeSettingsEndWithTheSound();
    TestSequenceWithoutNotesEndsAtItsStart();
    TestLoopStartSendsTheVolumeTheGameRestores();
    TestLoopMarksAPassThatPlaysAsTheNext();
    TestLoopDoesNotStrikeATiedNoteAgain();
    TestLoopThatNoPassRepeatsKeepsItsFirstPass();
    TestPassOnAnotherPartPlaysAsTheNext();
    TestSequenceThatFinishesHasNoLoop();
    TestVariableOptionSetsAVariableBeforeTheStart();
    TestRetryWithoutAWaitIsNoLoop();
    TestVariablesStartAsTheGamesDo();
    TestTrackStartsNoMoreNotesATickThanTheGameHasVoices();
    TestTrackPlayingNotesWithoutWaitingIsStopped();
    TestSoundsPanCurveAndModeAreReported();
    TestTempoAndSweepFollowTheBuild();
    TestMonophonicAndVelocityRangeComeWith2008();
    TestRegionsNoteOffTypeAndKeyGroupComeWith2008();
    TestMainDolShowsTheBuild();
    TestMainDolIsFoundBesideTheFiles();
    TestBuildOf2006CountsTicksIn416ths();
    TestBuildOf2006HasNoTimebase();
    TestBuildOf2006RaisesPan1AndHasOnlyDualMode();
    TestBuildOf2006StopsAReleaseAtVolume0();
    TestBuildsFrom2008PlayTheBiquad();
    TestBiquadSwitchedLaterInTheFrameReachesItsNotes();
    TestLfoSetAfterANoteInItsTickReachesItsPreset();
    TestLfoChangedAtDepth0IsReportedWhenHeard();
    TestDeepTremoloIsKeptTo6dB();
    TestNoteReleasedInItsAttackKeepsTheGamesLevel();
    TestTrackWaitingForeverEnds();
    TestHeldNoteHoldsUntilStopped();
    TestLoopUnrollingKeepsTheStream();
    TestSfzLoopUnrollingKeepsTheStream();
    TestOneShotWaveStaysAsItIs();
    TestSoundFontWriterOutputIsValid();
    TestSoundFontWithoutSamplesHasSampleData();
    TestBalanceModeKeepsStereoWavesAt3DbDown();
    TestMidiWriterLayersMarkersAndBendRange();
    TestMidiWriterGivesPartsChannelsOfTheirOwn();
    TestMidiWriterHasAtLeast7680TicksPerQuarterNote();
    TestMidiEventsGoOnSoundFrames();
    TestMidiWriterSplitsLongDeltaTimes();
    TestMidiBendUpByAWholeRangeIsExact();
    TestMidiWriterSendsTheFilterToEveryLayer();
    TestMidiWriterListsChannel10Presets();
    TestWavFileIsMonoPcm16();
    TestSfzFileHasTheTracksPresetsAndFilter();
    TestSfzCommentKeepsTheNameOnItsLine();
    TestFileNameKeepsValidNames();
    TestFileNameReplacesCharactersWindowsForbids();
    TestFileNameAvoidsDeviceNames();

    if (argc > 1)
    {
        TestArchiveConvertsEverySequence(argv[1]);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("all checks passed");

    return 0;
}
