// SPDX-License-Identifier: MIT

// NW4R SND's build, from a game's main.dol, and the settings that it plays with (see snd_build.h).

#include "snd_build.h"

#include <array>
#include <cstddef>
#include <system_error>

#include "ntdlib/nwsnd/console.h"
#include "ntdlib/nwsnd/tables.h"
#include "ntdlib/nwsnd/util.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/filter.h"

namespace revosf2
{

namespace fs = std::filesystem;
namespace nwsnd = ntdlib::nwsnd;
namespace sequence_export = ntdlib::sequence_export;

namespace
{

// The 0xd8 arguments that leave the track low-pass on: 0 to 57 in every build (see sequence_export::LowPassIndex).
constexpr std::size_t kLowPassArguments = 58;

// The track low-pass of the builds from November 2008, for each 0xd8 argument that leaves it on.
struct LowPassTables
{
    std::array<uint16_t, kLowPassArguments> cutoff;    // SoundFont cutoff, in absolute cents
    std::array<int16_t, kLowPassArguments> sfz_cutoff; // SFZ cutoff (see sequence_export::SfzLowPassCutoff)
    std::array<uint16_t, kLowPassArguments> hz;        // the game's cutoff
};

// The build of November 20 2008 takes the cutoff from a table (Util::CalcLpfFreq). The table is
// nwsnd::tables::kLpfFreq, and the arithmetic is the Wii U's (nwsnd::LpfFreqIndex). Earlier builds compute the cutoff
// (nwsnd::RevolutionPolicy::LpfFreq). Fitted at each of kLpfFreq's cutoffs, the Wii's one-pole (AXGetLpfCoefs) gives
// the Wii U's SoundFont and SFZ cutoffs. These builds therefore use the Wii U's tables.
LowPassTables November2008LowPass()
{
    LowPassTables tables{};
    for (std::size_t argument = 0; argument < kLowPassArguments; ++argument)
    {
        // The track gives (argument - 64) / 64, and the channel adds 1.
        const float value = static_cast<float>(static_cast<int>(argument) - 0x40) * 0.015625f + 1.0f;
        const auto step = static_cast<std::size_t>(nwsnd::LpfFreqIndex<nwsnd::CafePolicy>(value));
        tables.cutoff[argument] = sequence_export::kCafeFilters.low_pass_cutoff[step];
        tables.sfz_cutoff[argument] = sequence_export::kCafeFilters.sfz_low_pass_cutoff[step];
        tables.hz[argument] = nwsnd::tables::kLpfFreq[step];
    }

    return tables;
}

// The Wii's filter tables with the low-pass of `low_pass`.
sequence_export::FilterTables WithLowPass(const LowPassTables& low_pass)
{
    sequence_export::FilterTables filters = sequence_export::kRevolutionFilters;
    filters.low_pass_cutoff = low_pass.cutoff;
    filters.sfz_low_pass_cutoff = low_pass.sfz_cutoff;
    filters.low_pass_hz = low_pass.hz;

    return filters;
}

// The filter tables of the builds from November 2008.
const sequence_export::FilterTables& November2008Filters()
{
    static const LowPassTables kLowPass = November2008LowPass();
    static const sequence_export::FilterTables kFilters = WithLowPass(kLowPass);

    return kFilters;
}

} // namespace

std::optional<SndBuild> ReadSndBuild(std::span<const uint8_t> dol)
{
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol, {.skip_damaged_sections = true});
    if (!microcode)
    {
        return std::nullopt;
    }

    SndBuild build = ntdlib::FindSndBuild(dol, {.check_day = true});
    build.october_2006 = ntdlib::IdentifyBuild(*microcode) == ntdlib::AxBuild::kOctober2006;

    return build;
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

bool SinceNovember2008(const SndBuild& build)
{
    return build.date >= 20081120;
}

sequence_export::Settings SettingsFor(const SndBuild& build)
{
    sequence_export::Settings settings = sequence_export::RevolutionSettings(build);
    if (SinceNovember2008(build))
    {
        settings.filters = &November2008Filters();
    }

    return settings;
}

} // namespace revosf2
