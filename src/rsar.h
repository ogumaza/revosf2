// SPDX-License-Identifier: MIT

// NintendoWare for Revolution (NW4R) sound format readers: BRSAR archives and their RSEQ, RBNK, RWAR and RWAV files.
// Layouts follow NW4R SND's readers as linked into Wii Sports (2007), with the fields that the formats' later versions
// add. The WSF project reads them the same way.
//
// Archives come in two generations, and so do their banks. In RSAR 1.1, a bank (RBNK 1.1) describes its waves in a WAVE
// block, and its wave data is the raw samples. From RSAR 1.4, a bank (RBNK 1.2) may instead have no WAVE block and an
// RWAR of RWAV files for its wave data. A group holds each file with its wave data. A game loads them together.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace revosf2
{

class Reader;

enum class WaveEncoding : uint8_t
{
    kPcm8 = 0,
    kPcm16 = 1,
    kDspAdpcm = 2,
};

// DSP-ADPCM predictor coefficients (8 pairs), and the decoder's history at the start: the two samples before the first.
struct DspAdpcmInfo
{
    std::array<int16_t, 16> coefs{};
    int16_t yn1 = 0, yn2 = 0;
};

// One wave channel's sample data and initial decoder state.
struct WaveChannel
{
    std::span<const uint8_t> data; // sample data from the channel's start to the end of the wave data
    std::optional<DspAdpcmInfo> adpcm;
};

// A wave's encoding, sample rate, loop bounds and channels: an RWAV file, or a wave info in a version 1.1 bank's WAVE
// block. The file gives the loop as DSP addresses: nibbles for DSP-ADPCM, with two header nibbles in each 8-byte frame
// of 14 samples. The reader turns them into samples. WaveFileReader::ReadWaveParam (main.dol 0x8004eca8) does the same.
struct Wave
{
    // Read an RWAV file. Its samples are in its DATA block.
    static Wave ParseFile(std::span<const uint8_t> rwav);

    // Read the wave info at `offset` in `info`, whose samples start at its data offset in `wave_data`.
    static Wave ParseInfo(const Reader& info, std::size_t offset, std::span<const uint8_t> wave_data);

    WaveEncoding encoding = WaveEncoding::kPcm16;
    bool loop = false;
    uint32_t sample_rate = 32000;
    uint32_t loop_start = 0; // first sample of the loop
    uint32_t loop_end = 0;   // one past the last sample played: the wave's length, looping or not
    std::vector<WaveChannel> channels;
};

// RWAV files in a wave archive (RWAR).
struct WaveArchive
{
    static WaveArchive Parse(std::span<const uint8_t> file);

    std::vector<std::span<const uint8_t>> waves;
};

// A bank's region: the wave a range of keys and velocities plays, and how (InstParam).
struct Region
{
    int32_t wave_index = 0; // into the bank's waves
    uint8_t attack = 127, decay = 127, sustain = 127, release = 127;
    uint8_t hold = 0;          // builds from 2008 only
    uint8_t note_off_type = 0; // 1: ignores the note-off at the end of a note's length (builds from 2008 only)
    uint8_t key_group = 0;     // the alternate assign: a new note of the group releases the others (2008 builds only)
    uint8_t original_key = 60;
    uint8_t volume = 127;
    uint8_t pan = 64;
    float tune = 1.0f;
};

// RBNK: a bank of instruments, each a tree of key and velocity tables leading to regions, and the waves they play.
class Bank
{
public:
    // Read a bank's instruments. For a version 1.1 bank, also read where its WAVE block describes its waves. `file` has
    // to outlive the bank.
    static Bank Parse(std::span<const uint8_t> file);

    // The number of programs the instrument table has, some of which may play nothing.
    uint32_t InstrumentCount() const
    {
        return static_cast<uint32_t>(instruments_.size());
    }

    // The region that plays `key` at `velocity` on `program`, or nullptr (BankFileReader::ReadInstInfo, main.dol
    // 0x80035d94): the instrument's reference, then a key table, then a velocity table, each a range table (the first
    // entry whose upper key is at least the key) or an index table (by key from its lowest). The chain ends at a
    // region. Any other type of reference, a key outside a table and a negative wave index play nothing.
    const Region* Find(uint32_t program, int key, int velocity) const;

    // True for a version 1.1 bank, whose WAVE block describes its waves.
    bool HasWaveBlock() const
    {
        return !wave_infos_.empty() || wave_block_;
    }

    // Read wave `index` of a version 1.1 bank, whose samples are in `wave_data`. Throw FormatError if it can't be read.
    Wave ReadWave(std::size_t index, std::span<const uint8_t> wave_data) const;

private:
    // The data types of a bank's references.
    enum : uint8_t
    {
        kNone = 0,
        kRegion = 1,
        kRangeTable = 2,
        kIndexTable = 3,
    };

    // A reference in the tree, resolved: a region (an index into regions_), a table (an index into tables_), or
    // nothing.
    struct Node
    {
        uint8_t type = kNone;
        uint32_t index = 0;
    };

    // A range table (keys holds each entry's upper key) or an index table (keys holds its lowest and highest key).
    struct Table
    {
        uint8_t type = kRangeTable;
        std::vector<uint8_t> keys;
        std::vector<Node> entries;
    };

    // Select a table's entry for `key`, or nullptr.
    const Node* Choose(const Node& node, int key) const;

    std::span<const uint8_t> file_;
    std::vector<Node> instruments_;
    std::vector<Table> tables_;
    std::vector<Region> regions_;
    bool wave_block_ = false;
    std::vector<std::size_t> wave_infos_; // offsets of a version 1.1 bank's wave infos
};

// RSEQ bytecode.
struct Sequence
{
    static Sequence Parse(std::span<const uint8_t> file);

    std::span<const uint8_t> data; // the commands, the base of a sound's start offset
};

enum class SoundType
{
    kSequence,
    kStream,
    kWaveSound,
    kUnknown
};

// Sound metadata. Missing names are generated from type and index, e.g. SEQ_12. Control characters, line breaks and
// invalid UTF-8 bytes become '?'. The same goes for other item names.
struct SoundInfo
{
    std::string name;
    uint32_t file_id = 0;
    uint8_t volume = 127;
    uint8_t pan_mode = 1;  // 0 dual (pan stereo channels from their sides), 1 balance (the only mode before RSAR 1.2)
    uint8_t pan_curve = 0; // 0 is the default square-root law
    SoundType type = SoundType::kUnknown;

    // Sequence sounds only.
    uint32_t bank = 0xffffffff; // index into SoundArchive::Banks()
    uint32_t allocate_track_flags = 1;
    uint32_t start_offset = 0;
};

// Bank name and file ID. Unnamed banks use BANK_<index>.
struct BankInfo
{
    std::string name;
    uint32_t file_id = 0;
};

// RSAR sound and bank tables, with the files that groups hold in the archive.
class SoundArchive
{
public:
    // Read archive tables and locate the files that groups hold. Throw FormatError if the tables cannot be read.
    static SoundArchive Load(std::vector<uint8_t> bytes);

    // The archive's version, such as 0x0104 for RSAR 1.4.
    uint16_t Version() const
    {
        return version_;
    }

    const std::vector<SoundInfo>& Sounds() const
    {
        return sounds_;
    }

    const std::vector<BankInfo>& Banks() const
    {
        return banks_;
    }

    // Return a file's contents, from the first group that holds it and its wave data inside the archive, or else the
    // first that holds the file, or an empty span if none does.
    std::span<const uint8_t> FileData(uint32_t file_id) const;

    // Return the wave data that goes with FileData, or an empty span if no group holds both inside the archive.
    std::span<const uint8_t> WaveData(uint32_t file_id) const;

    // True if no group holds the file inside the archive, and one would if the archive weren't cut short: it extends
    // past the end.
    bool FileMissing(uint32_t file_id) const
    {
        return file_id < files_.size() && files_[file_id].missing;
    }

    // True if the file has wave data that no group holds inside the archive, and one would if the archive weren't cut
    // short.
    bool WaveDataMissing(uint32_t file_id) const
    {
        return file_id < files_.size() && files_[file_id].wave_data_missing;
    }

    // True if the archive is shorter than the size in its header.
    bool Truncated() const
    {
        return truncated_;
    }

    // True if the archive's name table is damaged. Its sounds and banks then go by number.
    bool NamesDamaged() const
    {
        return names_damaged_;
    }

private:
    // A file's place in the archive.
    struct FileEntry
    {
        std::size_t offset = 0;
        uint32_t size = 0;
        std::size_t wave_data_offset = 0;
        uint32_t wave_data_size = 0;
        bool found = false;             // a group holds the file inside the archive
        bool wave_data_found = false;   // and its wave data
        bool missing = false;           // a group holds the file past the end of the archive
        bool wave_data_missing = false; // a group holds its wave data past the end of the archive
    };

    // Read tables through `r`, and locate the files groups hold. Throw FormatError if the tables cannot be read.
    void ReadTables(const Reader& r);

    std::vector<uint8_t> bytes_;
    uint16_t version_ = 0;
    std::vector<SoundInfo> sounds_;
    std::vector<BankInfo> banks_;
    std::vector<FileEntry> files_;
    bool truncated_ = false;
    bool names_damaged_ = false;
};

} // namespace revosf2
