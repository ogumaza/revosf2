// SPDX-License-Identifier: MIT

// revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont and SFZ.
//
//     revosf2 [options] <archive.brsar> [<archive.brsar> ...]
//
// Output goes beside each archive. Windows accepts files dropped on the executable; macOS uses Revosf2.app.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/frame.h"

#include "snd_build.h"

namespace fs = std::filesystem;
namespace frame = ntdlib::sequence_export;
using ntdlib::rsar::SoundArchive;
using ntdlib::sequence_export::PerformOptions;
using revosf2::FindMainDol;
using revosf2::ReadSndBuild;
using revosf2::SettingsFor;
using revosf2::SinceNovember2008;
using revosf2::SndBuild;

namespace
{

// The rules that `build` plays by.
const char* RulesOf(const SndBuild& build)
{
    return build.October2006()        ? "builds of 2006"
           : SinceNovember2008(build) ? "builds from November 2008"
           : build.SinceJune2008()    ? "builds from June 2008"
           : build.Since2008()        ? "builds from March 2008"
                                      : "builds of 2007";
}

// The build of NW4R SND that plays `archive`, from `input`: the one that the game's main.dol shows. That's `dol_build`,
// from --dol, or else the main.dol of the extracted disc that holds the archive. Without one, the archive's version
// gives the build. Say which unless `quiet` is set. `dol_path` is --dol's file.
std::optional<SndBuild> ArchiveBuild(const fs::path& input, const SoundArchive& archive, bool quiet,
                                     const std::string& dol_path, const std::optional<SndBuild>& dol_build)
{
    const std::string name = frame::Utf8(input.filename());
    std::optional<SndBuild> build = dol_build;
    std::string dol = dol_path;
    if (!build)
    {
        if (const std::optional<fs::path> found = FindMainDol(input))
        {
            const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(*found);
            build = bytes ? ReadSndBuild(*bytes) : std::nullopt;
            dol = frame::Utf8(*found);
            if (!bytes)
            {
                std::fprintf(stderr, "warning: can't read %s: %s plays as its version shows\n", dol.c_str(),
                             name.c_str());
            }
            else if (!build)
            {
                std::fprintf(stderr, "warning: %s holds no AX microcode: %s plays as its version shows\n", dol.c_str(),
                             name.c_str());
            }
        }
    }

    // A main.dol that was found but can't give the build has had its warning.
    if (!quiet && build)
    {
        std::fprintf(stderr, "note: %s plays as NW4R SND's %s do, by the game's main.dol (%s)\n", name.c_str(),
                     RulesOf(*build), dol.c_str());
    }
    else if (!quiet && dol.empty() && archive.Version() < 0x0104)
    {
        std::fprintf(stderr,
                     "note: no main.dol found for %s: it plays as NW4R SND's builds of 2007 do (--dol gives the game's "
                     "main.dol)\n",
                     name.c_str());
    }

    return build;
}

} // namespace

int main(int argc, char** argv)
{
    ntdlib::sequence_export::ArchiveConverter<SoundArchive> converter;
    converter.program = "revosf2";
    converter.version_line = "revosf2 " REVOSF2_VERSION;
    converter.description =
        "revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont 2 and\n"
        "SFZ.\n";
    converter.extension = ".brsar";
    converter.seed_option = "initial game random state (default 0x12345678)";
    converter.extra_options =
        "  --dol FILE     the game's main.dol, for the build of the game's sound library\n"
        "                 (default: sys/main.dol of the extracted disc that holds the\n"
        "                 archive)\n";

    // --dol names the game's main.dol. The build that it shows plays every archive.
    std::string dol;
    std::optional<SndBuild> dol_build;
    converter.option = [&dol](const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
    {
        if (option == "--dol")
        {
            dol = reader.Value();
            return true;
        }

        return frame::VarOption(option, reader, perform);
    };

    converter.before_inputs = [&]()
    {
        if (!dol.empty())
        {
            const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(frame::PathFromUtf8(dol));
            dol_build = bytes ? ReadSndBuild(*bytes) : std::nullopt;
            if (!dol_build)
            {
                throw std::runtime_error(bytes ? dol + " holds no AX microcode: it isn't a game's main.dol"
                                               : "can't read " + dol);
            }
        }
    };

    // Each archive plays by the build that its main.dol shows, or else by the one its version gives.
    converter.archive_settings =
        [&](const fs::path& input, const SoundArchive& archive, const frame::Options<PerformOptions>& options)
    {
        const std::optional<SndBuild> build = ArchiveBuild(input, archive, options.quiet, dol, dol_build);
        return SettingsFor(build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive.Version())));
    };

    return ntdlib::sequence_export::RunArchiveConverter(argc, argv, converter);
}
