// SPDX-License-Identifier: MIT

// NW4R SND's build, from an archive's version or a game's main.dol (see snd_build.h).

#include "snd_build.h"

#include <cstddef>
#include <string_view>
#include <system_error>

namespace revosf2
{

namespace
{

namespace fs = std::filesystem;

// A DOL's header: the file offsets (0x00), load addresses (0x48) and sizes (0x90) of its 7 text and 11 data sections.
constexpr std::size_t kDolHeaderSize = 0x100;
constexpr std::size_t kDolSections = 18;

// AX's microcode fills the DSP's instruction RAM: 4096 words.
constexpr std::size_t kMicrocodeBytes = 0x2000;

uint16_t Be16(std::span<const uint8_t> data, std::size_t at)
{
    return static_cast<uint16_t>(data[at] << 8 | data[at + 1]);
}

uint32_t Be32(std::span<const uint8_t> data, std::size_t at)
{
    return static_cast<uint32_t>(Be16(data, at)) << 16 | Be16(data, at + 2);
}

// True if `code` starts as AX's microcode does: two NOPs at the reset vector, then seven jumps (0x029F) to the top of
// instruction RAM (0x0Fxx).
bool IsAxVectors(std::span<const uint8_t> code)
{
    if (Be32(code, 0) != 0)
    {
        return false;
    }

    for (std::size_t i = 0; i < 7; i++)
    {
        const std::size_t jump = 4 + 4 * i;
        if (Be16(code, jump) != 0x029f || (Be16(code, jump + 2) & 0xff00) != 0x0f00)
        {
            return false;
        }
    }

    return true;
}

// True for the microcode of AX of October 2006. It transfers 0x120-byte parameter blocks (`si @DSBL, #0x120`), and
// never 0x140-byte ones as every later AX does.
bool TransfersOldBlocks(std::span<const uint8_t> code)
{
    bool old_blocks = false;
    bool new_blocks = false;
    for (std::size_t at = 0; at + 4 <= code.size(); at += 2)
    {
        if (Be16(code, at) == 0x16cb)
        {
            old_blocks = old_blocks || Be16(code, at + 2) == 0x0120;
            new_blocks = new_blocks || Be16(code, at + 2) == 0x0140;
        }
    }

    return old_blocks && !new_blocks;
}

// The date in NW4R SND's banner, as YYYYMMDD, or 0 without one. The banner reads like "<< NW4R    - SND \tfinal
// build: Jun  8 2007 11:17:15 (0x4199_60831) >>", with the date in the form of C's __DATE__.
uint32_t BannerDate(std::span<const uint8_t> dol)
{
    const std::string_view text(reinterpret_cast<const char*>(dol.data()), dol.size());
    const std::size_t banner = text.find("<< NW4R    - SND ");
    const std::size_t build = banner == std::string_view::npos ? banner : text.find("build: ", banner);
    if (build == std::string_view::npos || build - banner > 32 || text.size() - build < 18)
    {
        return 0;
    }

    constexpr std::string_view kMonths = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const std::string_view date = text.substr(build + 7, 11);
    const std::size_t month = kMonths.find(date.substr(0, 3));
    auto digit = [&date](std::size_t i) -> int
    {
        return date[i] == ' ' ? 0 : (date[i] >= '0' && date[i] <= '9' ? date[i] - '0' : -1);
    };
    for (const std::size_t i : {4u, 5u, 7u, 8u, 9u, 10u})
    {
        if (digit(i) < 0 || (i >= 7 && date[i] == ' '))
        {
            return 0;
        }
    }
    if (month == std::string_view::npos || month % 3 != 0)
    {
        return 0;
    }

    const int year = digit(7) * 1000 + digit(8) * 100 + digit(9) * 10 + digit(10);
    const int day = digit(4) * 10 + digit(5);
    return static_cast<uint32_t>(year * 10000 + (static_cast<int>(month) / 3 + 1) * 100 + day);
}

} // namespace

SndBuild BuildOfVersion(uint16_t version)
{
    SndBuild build;
    build.date = version >= 0x0104 ? 20080620 : 0;

    return build;
}

std::optional<SndBuild> ReadSndBuild(std::span<const uint8_t> dol)
{
    if (dol.size() < kDolHeaderSize)
    {
        return std::nullopt;
    }

    // AX's microcode is data in one of the sections, on an 8-byte boundary.
    for (std::size_t s = 0; s < kDolSections; s++)
    {
        const std::size_t offset = Be32(dol, 4 * s);
        const std::size_t size = Be32(dol, 0x90 + 4 * s);
        if (size == 0 || offset < kDolHeaderSize || offset > dol.size() || size > dol.size() - offset)
        {
            continue;
        }

        const std::span<const uint8_t> section = dol.subspan(offset, size);
        for (std::size_t at = 0; at + kMicrocodeBytes <= section.size(); at += 8)
        {
            const std::span<const uint8_t> code = section.subspan(at, kMicrocodeBytes);
            if (IsAxVectors(code))
            {
                return SndBuild{BannerDate(dol), TransfersOldBlocks(code)};
            }
        }
    }

    return std::nullopt;
}

std::optional<fs::path> FindMainDol(const fs::path& archive)
{
    std::error_code error;
    const fs::path absolute = fs::absolute(archive, error);
    if (error)
    {
        return std::nullopt;
    }

    for (fs::path folder = absolute.parent_path(); folder.has_relative_path(); folder = folder.parent_path())
    {
        fs::path dol = folder.parent_path() / "sys" / "main.dol";
        if (folder.filename() == "files" && fs::is_regular_file(dol, error))
        {
            return dol;
        }
    }

    return std::nullopt;
}

} // namespace revosf2
