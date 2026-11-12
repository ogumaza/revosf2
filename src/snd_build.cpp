// SPDX-License-Identifier: MIT

// The sound library that a game's main.dol links, and the settings that it gives (see snd_build.h).

#include "snd_build.h"

#include <cstdint>
#include <optional>
#include <span>

#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_build.h"

namespace revosf2
{

std::optional<SoundLibrary> ReadSoundLibrary(std::span<const uint8_t> dol)
{
    const std::optional<ntdlib::Microcode> microcode = ntdlib::FindMicrocode(dol, {.skip_damaged_sections = true});
    if (!microcode)
    {
        return std::nullopt;
    }

    const ntdlib::AxBuild ax = ntdlib::IdentifyBuild(*microcode);
    SoundLibrary library;
    library.build = ntdlib::SndBuildWithAx(ntdlib::FindSndBuild(dol, {.check_day = true}), ax);
    library.voices = ax == ntdlib::AxBuild::kOctober2006 ? 64 : 96;

    return library;
}

ntdlib::sequence_export::Settings ArchiveSettings(const std::optional<SoundLibrary>& library, uint16_t rsar_version)
{
    ntdlib::sequence_export::Settings settings = ntdlib::sequence_export::RevolutionSettings(
        library ? library->build : ntdlib::sequence_export::BuildOfRsarVersion(rsar_version));
    if (library)
    {
        settings.voices = library->voices;
    }

    return settings;
}

} // namespace revosf2
