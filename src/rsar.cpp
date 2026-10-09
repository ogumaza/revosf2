// SPDX-License-Identifier: MIT

// Sound archive and file readers (see rsar.h).

#include "rsar.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "binary.h"

namespace revosf2
{

namespace
{

// The longest name read from the string table.
constexpr std::size_t kMaxName = 1024;

// DSP-ADPCM frames: 8 bytes, a header byte and 14 samples, addressed in nibbles.
constexpr uint32_t kAdpcmFrameNibbles = 16;
constexpr uint32_t kAdpcmFrameSamples = 14;

// Check the file magic (RSAR, RWAV, etc.) and the Wii's byte order, and return the file's version.
uint16_t CheckHeader(const Reader& r, const char* magic)
{
    if (r.Size() < 0x10)
    {
        throw FormatError(std::string("too short to be an ") + magic + " file");
    }
    if (!r.Magic(0, magic))
    {
        throw FormatError(std::string("bad magic, expected ") + magic);
    }
    if (r.U16(4) != 0xFEFF)
    {
        throw FormatError("unsupported byte order");
    }

    return r.U16(6);
}

// The sample a DSP address points at, in the units of `encoding`: nibbles for DSP-ADPCM, whose frames start with two
// header nibbles, and samples for PCM. An address on a header counts as the frame's first sample.
uint32_t SampleAt(uint32_t address, WaveEncoding encoding)
{
    if (encoding != WaveEncoding::kDspAdpcm)
    {
        return address;
    }

    return address / kAdpcmFrameNibbles * kAdpcmFrameSamples + std::max(address % kAdpcmFrameNibbles, 2u) - 2;
}

// Read a wave info at `offset` whose channels' sample offsets count from `channel_base`.
Wave ReadWaveInfo(const Reader& info, std::size_t offset, std::span<const uint8_t> channel_base)
{
    Wave w;
    const uint8_t encoding = info.U8(offset);
    if (encoding > 2)
    {
        throw FormatError("unknown wave encoding " + std::to_string(encoding));
    }

    // The loop start is the first sample's address, and the end the last sample's. ReadWaveParam (main.dol 0x8004eca8)
    // makes the end one past it.
    w.encoding = static_cast<WaveEncoding>(encoding);
    w.loop = info.U8(offset + 1) != 0;
    const uint8_t channels = info.U8(offset + 2);
    w.sample_rate = static_cast<uint32_t>(info.U8(offset + 3)) << 16 | info.U16(offset + 4);
    w.loop_start = SampleAt(info.U32(offset + 8), w.encoding);
    w.loop_end = SampleAt(info.U32(offset + 0xC), w.encoding) + 1;

    // Each channel: its samples' offset, and its ADPCM info's offset from the wave info (0 for none).
    const std::size_t table = offset + info.U32(offset + 0x10);
    for (uint8_t c = 0; c < channels; c++)
    {
        const std::size_t channel = offset + info.U32(table + c * 4u);
        const uint32_t samples = info.U32(channel);
        if (samples > channel_base.size())
        {
            throw FormatError("wave channel " + std::to_string(c) + " starts past the wave data");
        }

        WaveChannel ch;
        ch.data = channel_base.subspan(samples);
        if (const uint32_t adpcm = info.U32(channel + 4); adpcm != 0 && w.encoding == WaveEncoding::kDspAdpcm)
        {
            const std::size_t a = offset + adpcm;
            DspAdpcmInfo d;
            for (int k = 0; k < 16; k++)
            {
                d.coefs[k] = info.S16(a + k * 2u);
            }

            d.yn1 = info.S16(a + 0x24);
            d.yn2 = info.S16(a + 0x26);
            ch.adpcm = d;
        }

        w.channels.push_back(ch);
    }

    return w;
}

// Decode the first UTF-8 character in nonempty `s`. Return its code point and byte length, or length 0 for invalid
// UTF-8.
std::pair<uint32_t, std::size_t> Utf8Character(std::string_view s)
{
    const auto byte = [&](std::size_t i)
    {
        return static_cast<uint8_t>(s[i]);
    };

    const uint8_t lead = byte(0);
    const std::size_t length = lead < 0x80   ? 1
                               : lead < 0xc2 ? 0
                               : lead < 0xe0 ? 2
                               : lead < 0xf0 ? 3
                               : lead < 0xf5 ? 4
                                             : 0;
    if (length == 0 || length > s.size())
    {
        return {0, 0};
    }

    uint32_t c = length == 1 ? lead : lead & (0x7f >> length);
    for (std::size_t i = 1; i < length; i++)
    {
        if ((byte(i) & 0xc0) != 0x80)
        {
            return {0, 0};
        }

        c = c << 6 | (byte(i) & 0x3f);
    }

    // Reject overlong encodings, UTF-16 surrogates and code points above U+10FFFF.
    constexpr uint32_t kShortest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (c < kShortest[length] || (c >= 0xd800 && c < 0xe000) || c > 0x10ffff)
    {
        return {0, 0};
    }

    return {c, length};
}

// Replace control characters, line/paragraph separators and invalid UTF-8 bytes with '?'. These can disrupt terminal
// output and the macOS app's parser: AppleScript splits on U+2029 and falls back to another encoding if any byte is
// invalid UTF-8. macOS also requires valid UTF-8 file names.
std::string PrintableName(std::string_view name)
{
    std::string printable;
    for (std::size_t i = 0; i < name.size();)
    {
        const auto [c, length] = Utf8Character(name.substr(i));
        const bool control = c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x2028 || c == 0x2029;
        printable += length == 0 || control ? "?" : name.substr(i, length);
        i += std::max<std::size_t>(length, 1);
    }

    return printable;
}

// Read the SYMB block's string table at `symb`: a table of offsets, from the block's payload, to the names. Items refer
// to the names by index.
std::vector<std::string> ReadStrings(const Reader& r, std::size_t symb)
{
    const std::size_t base = symb + 8;
    const std::size_t table = base + r.U32(base);
    const uint32_t count = r.U32(table);

    // A name takes a byte at least. A table of more entries than the archive has bytes is damaged.
    if (count > r.Size())
    {
        throw FormatError("invalid string table");
    }

    std::vector<std::string> strings;
    for (uint32_t i = 0; i < count; i++)
    {
        strings.push_back(PrintableName(r.CString(base + r.U32(table + 4 + i * 4), kMaxName)));
    }

    return strings;
}

} // namespace

Wave Wave::ParseFile(std::span<const uint8_t> rwav)
{
    // An INFO block holding the wave info, and a DATA block holding the samples. The channels' offsets count from the
    // samples. (The wave info's data offset points at the DATA block's header.)
    Reader r(rwav);
    CheckHeader(r, "RWAV");
    const uint32_t info = r.U32(0x10);
    const uint32_t data = r.U32(0x18);
    if (!r.Magic(info, "INFO") || !r.Magic(data, "DATA"))
    {
        throw FormatError("no INFO or DATA block");
    }

    return ReadWaveInfo(r, info + 8, r.Sub(data + 8, rwav.size() - (data + 8)));
}

Wave Wave::ParseInfo(const Reader& info, std::size_t offset, std::span<const uint8_t> wave_data)
{
    const uint32_t data_offset = info.U32(offset + 0x14);
    if (data_offset > wave_data.size())
    {
        throw FormatError("samples start past the wave data");
    }

    return ReadWaveInfo(info, offset, wave_data.subspan(data_offset));
}

WaveArchive WaveArchive::Parse(std::span<const uint8_t> file)
{
    // A TABL block lists each wave as a reference to it, from the DATA block's start, and its size.
    Reader r(file);
    CheckHeader(r, "RWAR");
    const uint32_t table = r.U32(0x10);
    const uint32_t data = r.U32(0x18);
    if (!r.Magic(table, "TABL") || !r.Magic(data, "DATA"))
    {
        throw FormatError("no TABL or DATA block");
    }

    WaveArchive war;
    const uint32_t count = r.U32(table + 8);
    for (uint32_t i = 0; i < count; i++)
    {
        const std::size_t e = table + 0xC + i * std::size_t{12};
        war.waves.push_back(r.Sub(data + std::size_t{ReadRef(r, e).offset}, r.U32(e + 8)));
    }

    return war;
}

Bank Bank::Parse(std::span<const uint8_t> file)
{
    Reader r(file);
    Bank bank;
    bank.file_ = file;
    const uint16_t version = CheckHeader(r, "RBNK");
    const uint32_t data = r.U32(0x10);
    if (!r.Magic(data, "DATA"))
    {
        throw FormatError("no DATA block");
    }

    // The instrument table: a count, then a reference for each program, from the table's start. Tables and regions that
    // several references share are read once. A bank whose tables all point at one entry then stays small.
    const std::size_t base = data + 8;
    std::map<std::pair<uint8_t, uint32_t>, uint32_t> seen;
    const auto read_node = [&](std::size_t ref_at, int depth, const auto& self) -> Node
    {
        const Reference ref = ReadRef(r, ref_at);
        if (!ref.IsOffset() || ref.data_type == kNone)
        {
            return Node{kNone, 0};
        }

        // NW4R selects at most two levels deep, a key and then a velocity, and has no other types.
        if (ref.data_type != kRegion && (depth >= 2 || ref.data_type > kIndexTable))
        {
            return Node{kNone, 0};
        }
        if (const auto known = seen.find({ref.data_type, ref.offset}); known != seen.end())
        {
            return Node{ref.data_type, known->second};
        }

        const std::size_t at = base + ref.offset;
        if (ref.data_type == kRegion)
        {
            // BankFileReader::ReadInstInfo: a bank older than version 1.1 has no volume or tune.
            Region region;
            region.wave_index = r.S32(at);
            region.attack = r.U8(at + 4);
            region.decay = r.U8(at + 5);
            region.sustain = r.U8(at + 6);
            region.release = r.U8(at + 7);
            region.hold = r.U8(at + 8);
            region.note_off_type = r.U8(at + 0xA);
            region.key_group = r.U8(at + 0xB);
            region.original_key = r.U8(at + 0xC);
            region.pan = r.U8(at + 0xE);
            if (version >= 0x0101)
            {
                region.volume = r.U8(at + 0xD);
                region.tune = r.F32(at + 0x10);
            }

            const auto index = static_cast<uint32_t>(bank.regions_.size());
            bank.regions_.push_back(region);
            seen[{ref.data_type, ref.offset}] = index;

            return Node{kRegion, index};
        }

        // A range table: a count and each entry's upper key, then references from a multiple of 4 bytes on. An index
        // table: its lowest and highest key, two padding bytes, then a reference for each key.
        Table table;
        table.type = ref.data_type;
        std::size_t refs = 0;
        std::size_t entries = 0;
        if (ref.data_type == kRangeTable)
        {
            entries = r.U8(at);
            for (std::size_t i = 0; i < entries; i++)
            {
                table.keys.push_back(r.U8(at + 1 + i));
            }

            refs = at + ((entries + 1 + 3) & ~std::size_t{3});
        }
        else
        {
            const uint8_t lowest = r.U8(at);
            const uint8_t highest = r.U8(at + 1);
            table.keys = {lowest, highest};
            entries = highest >= lowest ? std::size_t{highest} - lowest + 1 : 0;
            refs = at + 4;
        }

        const auto index = static_cast<uint32_t>(bank.tables_.size());
        seen[{ref.data_type, ref.offset}] = index;
        bank.tables_.push_back(table);
        for (std::size_t i = 0; i < entries; i++)
        {
            const Node entry = self(refs + i * 8, depth + 1, self);
            bank.tables_[index].entries.push_back(entry);
        }

        return Node{ref.data_type, index};
    };

    const uint32_t count = r.U32(base);
    if (count > file.size())
    {
        throw FormatError("invalid instrument table");
    }
    for (uint32_t i = 0; i < count; i++)
    {
        bank.instruments_.push_back(read_node(base + 4 + i * std::size_t{8}, 0, read_node));
    }

    // A version 1.1 bank's WAVE block holds a reference to each wave info, from the table's start. A later bank has
    // none, and its wave data is an RWAR.
    const uint32_t wave = r.U16(0x0E) >= 2 ? r.U32(0x18) : 0;
    if (wave != 0 && r.Magic(wave, "WAVE"))
    {
        bank.wave_block_ = true;
        const std::size_t wave_base = wave + 8;
        const uint32_t waves = r.U32(wave_base);
        if (waves > file.size())
        {
            throw FormatError("invalid wave table");
        }
        for (uint32_t i = 0; i < waves; i++)
        {
            bank.wave_infos_.push_back(wave_base + ReadRef(r, wave_base + 4 + i * std::size_t{8}).offset);
        }
    }

    return bank;
}

const Bank::Node* Bank::Choose(const Node& node, int key) const
{
    // BankFileReader::GetReferenceToSubRegion (main.dol 0x80035f40).
    switch (node.type)
    {
    case kRegion:
        return &node;

    case kRangeTable:
        {
            const Table& table = tables_[node.index];
            for (std::size_t i = 0; i < table.entries.size(); i++)
            {
                if (key <= table.keys[i])
                {
                    return &table.entries[i];
                }
            }

            return nullptr;
        }

    case kIndexTable:
        {
            const Table& table = tables_[node.index];
            if (key < table.keys[0] || key > table.keys[1] || table.entries.empty())
            {
                return nullptr;
            }

            return &table.entries[static_cast<std::size_t>(key - table.keys[0])];
        }

    default:
        return nullptr;
    }
}

const Region* Bank::Find(uint32_t program, int key, int velocity) const
{
    if (program >= instruments_.size())
    {
        return nullptr;
    }

    const Node* node = &instruments_[program];
    for (const int split : {key, velocity})
    {
        if (node->type == kNone)
        {
            return nullptr;
        }

        node = Choose(*node, split);
        if (!node)
        {
            return nullptr;
        }
    }

    if (node->type != kRegion || regions_[node->index].wave_index < 0)
    {
        return nullptr;
    }

    return &regions_[node->index];
}

Wave Bank::ReadWave(std::size_t index, std::span<const uint8_t> wave_data) const
{
    if (index >= wave_infos_.size())
    {
        throw FormatError("no wave " + std::to_string(index) + " (the bank has " + std::to_string(wave_infos_.size()) +
                          ")");
    }

    return Wave::ParseInfo(Reader(file_), wave_infos_[index], wave_data);
}

Sequence Sequence::Parse(std::span<const uint8_t> file)
{
    // DATA holds the commands from an offset it gives, from the block's start.
    Reader r(file);
    CheckHeader(r, "RSEQ");
    const uint32_t data = r.U32(0x10);
    const uint32_t data_size = r.U32(0x14);
    if (!r.Magic(data, "DATA") || data_size < 0xC)
    {
        throw FormatError("no DATA block");
    }

    const uint32_t commands = r.U32(data + 8);
    if (commands > data_size)
    {
        throw FormatError("commands start past the DATA block");
    }

    Sequence s;
    s.data = r.Sub(data + std::size_t{commands}, data_size - commands);

    return s;
}

SoundArchive SoundArchive::Load(std::vector<uint8_t> bytes)
{
    SoundArchive a;
    a.bytes_ = std::move(bytes);
    const Reader r(a.bytes_);
    if (r.Size() >= 4 && r.Magic(0, "CSAR"))
    {
        throw FormatError("unsupported CSAR archive (3DS); expected RSAR (Wii)");
    }
    if (r.Size() >= 4 && r.Magic(0, "FSAR"))
    {
        throw FormatError("unsupported FSAR archive (Wii U/Switch); expected RSAR (Wii)");
    }
    if (r.Size() >= 4 && r.Magic(0, "SDAT"))
    {
        throw FormatError("unsupported SDAT archive (DS); expected RSAR (Wii)");
    }

    a.version_ = CheckHeader(r, "RSAR");

    // Read as much of a truncated archive as possible. Use the size in its header to detect truncation, and include
    // that in the error if its tables cannot be read.
    const uint32_t declared_size = r.U32(0x08);
    a.truncated_ = declared_size > a.bytes_.size();
    try
    {
        a.ReadTables(r);
    }
    catch (const FormatError& e)
    {
        if (!a.truncated_)
        {
            throw;
        }

        throw FormatError("truncated archive: " + std::to_string(a.bytes_.size()) + " of " +
                          std::to_string(declared_size) + " bytes; cannot read tables (" + e.what() + ")");
    }

    return a;
}

void SoundArchive::ReadTables(const Reader& r)
{
    const uint32_t symb = r.U32(0x10);
    const uint32_t symb_size = r.U32(0x14);
    const uint32_t info = r.U32(0x18);
    if (!r.Magic(info, "INFO"))
    {
        throw FormatError("no INFO block");
    }

    // Archives without names have no SYMB block. A damaged one leaves the sounds unnamed, rather than stopping the
    // archive.
    std::vector<std::string> strings;
    if (symb != 0 && symb_size != 0 && symb < r.Size() && r.Size() - symb >= 4 && r.Magic(symb, "SYMB"))
    {
        try
        {
            strings = ReadStrings(r, symb);
        }
        catch (const FormatError&)
        {
            names_damaged_ = true;
        }
    }

    auto name_of = [&](uint32_t string_id)
    {
        return string_id < strings.size() ? strings[string_id] : std::string();
    };

    // INFO starts with references, from its payload, to the sound, bank, player, file and group tables. Each table is a
    // count and a reference to each entry.
    const std::size_t base = info + 8;
    auto for_each_entry = [&](std::size_t table_ref, const char* name, auto read)
    {
        const Reference ref = ReadRef(r, table_ref);
        if (!ref.IsOffset())
        {
            throw FormatError(std::string("no ") + name + " table");
        }

        const std::size_t t = base + ref.offset;
        const uint32_t n = r.U32(t);
        for (uint32_t i = 0; i < n; i++)
        {
            read(i, base + ReadRef(r, t + 4 + i * std::size_t{8}).offset);
        }
    };

    // SoundArchiveFileReader::ReadSoundInfo (main.dol 0x800425a8) and ReadSeqSoundInfo (0x800427cc). Before version 1.1
    // the detail reference's data type gives the sound's type; from 1.1 a field does. Pan mode and curve came with
    // version 1.2.
    auto read_sound = [&](uint32_t i, std::size_t p)
    {
        SoundInfo s;
        s.file_id = r.U32(p + 4);
        s.volume = r.U8(p + 0x14);
        const Reference detail = ReadRef(r, p + 0x18);
        const uint8_t type = version_ >= 0x0101 ? r.U8(p + 0x16) : detail.data_type;
        s.type = type == 1   ? SoundType::kSequence
                 : type == 2 ? SoundType::kStream
                 : type == 3 ? SoundType::kWaveSound
                             : SoundType::kUnknown;
        if (version_ >= 0x0102)
        {
            s.pan_mode = r.U8(p + 0x28);
            s.pan_curve = r.U8(p + 0x29);
        }

        if (s.type == SoundType::kSequence && detail.IsOffset())
        {
            const std::size_t q = base + detail.offset;
            s.start_offset = r.U32(q);
            s.bank = r.U32(q + 4);
            s.allocate_track_flags = r.U32(q + 8);
        }

        // Generate a name from the sound's type and index if the archive has none.
        s.name = name_of(r.U32(p));
        if (s.name.empty())
        {
            const char* prefix = s.type == SoundType::kSequence    ? "SEQ_"
                                 : s.type == SoundType::kStream    ? "STRM_"
                                 : s.type == SoundType::kWaveSound ? "WSD_"
                                                                   : "SOUND_";
            s.name = prefix + std::to_string(i);
        }

        sounds_.push_back(std::move(s));
    };
    for_each_entry(base, "sound", read_sound);

    auto read_bank = [&](uint32_t i, std::size_t p)
    {
        const std::string name = name_of(r.U32(p));
        banks_.push_back({name.empty() ? "BANK_" + std::to_string(i) : name, r.U32(p + 4)});
    };
    for_each_entry(base + 8, "bank", read_bank);

    // Each file lists the places in groups where the archive keeps it: a group and an item of the group.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> positions;
    auto read_file = [&](uint32_t, std::size_t p)
    {
        std::vector<std::pair<uint32_t, uint32_t>>& places = positions.emplace_back();
        auto read_position = [&](uint32_t, std::size_t q)
        {
            places.emplace_back(r.U32(q), r.U32(q + 4));
        };
        for_each_entry(p + 0x14, "file position", read_position);
    };
    for_each_entry(base + 0x18, "file", read_file);
    files_.resize(positions.size());

    // A group holds its files, and after them their wave data. A group with an external file path is kept outside the
    // archive (SoundArchiveFileReader::ReadGroupInfo, main.dol 0x80042ac4, and ReadGroupItemInfo, 0x80042bd4).
    struct Group
    {
        bool internal = false;
        uint64_t offset = 0, wave_data_offset = 0;
        std::vector<std::size_t> items; // places of the items' entries
    };

    std::vector<Group> groups;
    auto read_group = [&](uint32_t, std::size_t p)
    {
        Group& g = groups.emplace_back();
        g.internal = !ReadRef(r, p + 8).IsOffset();
        g.offset = r.U32(p + 0x10);
        g.wave_data_offset = r.U32(p + 0x18);
        auto read_item = [&](uint32_t, std::size_t q)
        {
            g.items.push_back(q);
        };
        for_each_entry(p + 0x20, "group item", read_item);
    };
    for_each_entry(base + 0x20, "group", read_group);

    // Use the first group that holds a file and its wave data inside the archive, or else the first that holds the
    // file. Copies in different groups are the same.
    const auto inside = [&](uint64_t offset, uint64_t size)
    {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    };
    for (std::size_t id = 0; id < files_.size(); id++)
    {
        FileEntry& f = files_[id];
        for (const auto& [group, item] : positions[id])
        {
            if (group >= groups.size() || !groups[group].internal || item >= groups[group].items.size())
            {
                continue;
            }

            const Group& g = groups[group];
            const std::size_t q = g.items[item];
            const uint64_t offset = g.offset + r.U32(q + 4);
            const uint32_t size = r.U32(q + 8);
            const uint64_t wave_offset = g.wave_data_offset + r.U32(q + 0xC);
            const uint32_t wave_size = r.U32(q + 0x10);
            const bool file_inside = inside(offset, size);
            const bool wave_inside = inside(wave_offset, wave_size);
            f.missing = f.missing || !file_inside;
            f.wave_data_missing = f.wave_data_missing || !wave_inside;
            if (file_inside && (!f.found || (wave_inside && !f.wave_data_found)))
            {
                f.found = true;
                f.offset = static_cast<std::size_t>(offset);
                f.size = size;
                f.wave_data_found = wave_inside;
                f.wave_data_offset = wave_inside ? static_cast<std::size_t>(wave_offset) : 0;
                f.wave_data_size = wave_inside ? wave_size : 0;
            }
        }

        f.missing = f.missing && !f.found;
        f.wave_data_missing = f.wave_data_missing && !f.wave_data_found;
    }
}

std::span<const uint8_t> SoundArchive::FileData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].found)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].offset, files_[file_id].size);
}

std::span<const uint8_t> SoundArchive::WaveData(uint32_t file_id) const
{
    if (file_id >= files_.size() || !files_[file_id].wave_data_found)
    {
        return {};
    }

    return std::span<const uint8_t>(bytes_).subspan(files_[file_id].wave_data_offset, files_[file_id].wave_data_size);
}

} // namespace revosf2
