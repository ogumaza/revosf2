// SPDX-License-Identifier: MIT

// NW4R SND's build, from a game's main.dol (see snd_build.h).

#include "snd_build.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>

#include "ntdlib/sound_build.h"

namespace revosf2
{

namespace fs = std::filesystem;

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

} // namespace revosf2
