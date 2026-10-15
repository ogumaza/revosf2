// SPDX-License-Identifier: MIT

// revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont and SFZ.
//
//     revosf2 [options] <archive.brsar> [<archive.brsar> ...]
//
// Output goes beside each archive. An input can also be a game: a Wii disc image, an extracted disc or a WiiWare
// title's folder. Each of its sound archives converts into a folder of its own. Those folders go in a folder beside the
// game. Windows accepts files dropped on the executable; macOS uses Revosf2.app.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sequence_export/sfz.h"
#include "seqlib/file_name.h"
#include "xsflib/rip/tool.h"

#include "games.h"
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

using Converter = frame::Converter<PerformOptions, SoundArchive, frame::Conversion>;
using Options = frame::Options<PerformOptions>;
using UsedFolders = std::vector<std::pair<fs::path, std::string>>;

// The main.dol of a game given as an input: its bytes, and the game's path for the messages.
struct GameDol
{
    std::span<const uint8_t> bytes;
    std::string path;
};

// Sets the settings for the sequences of `archive`, read from `input`, and prints which build plays it unless `quiet`
// is set. `game` is the main.dol of the game that holds the archive, if the input was a game.
using PrepareArchive =
    std::function<void(const fs::path& input, const SoundArchive& archive, bool quiet, const GameDol* game)>;

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
// from --dol, or else the build of `game`, the main.dol of a game given as the input, or else the main.dol of the
// extracted disc that holds the archive. Without one, the archive's version gives the build. Say which unless `quiet`
// is set. `dol_path` is --dol's file.
std::optional<SndBuild> ArchiveBuild(const fs::path& input, const SoundArchive& archive, bool quiet,
                                     const std::string& dol_path, const std::optional<SndBuild>& dol_build,
                                     const GameDol* game)
{
    const std::string name = frame::Utf8(input.filename());
    std::optional<SndBuild> build = dol_build;
    std::string dol = dol_path;
    if (!build && game)
    {
        build = ReadSndBuild(game->bytes);
        dol = game->path;
        if (!build)
        {
            std::fprintf(stderr, "warning: the main.dol of %s holds no AX microcode: %s plays as its version shows\n",
                         dol.c_str(), name.c_str());
        }
    }
    else if (!build)
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

// Reads the sound archive in `bytes`, named `name`, from a game, and prints its warnings as frame::OpenArchive does for
// an archive's file.
SoundArchive OpenGameArchive(std::vector<uint8_t> bytes, const std::string& name)
{
    const std::size_t size = bytes.size();
    SoundArchive archive = SoundArchive::Load(std::move(bytes));
    if (archive.Truncated())
    {
        std::fprintf(stderr,
                     "warning: %s is truncated (%zu bytes): sounds whose data is missing are skipped, and notes whose "
                     "waves are missing are silent\n",
                     name.c_str(), size);
    }
    if (archive.NamesDamaged())
    {
        std::fprintf(stderr, "warning: %s has a damaged name table: its sounds are named by number\n", name.c_str());
    }

    return archive;
}

// Converts each sound archive of the game at `input` the way frame::ConvertInput converts a loose archive. Each goes in
// a folder of its own in the game's output folder. xsflib's ArchiveFolders names those folders the way the rip tools
// name them. The game's main.dol gives the build unless --dol gives one (`prepare`). `several` is set when the command
// line has more than one input. Returns 1 on any failure, otherwise 0.
int ConvertGame(const std::string& input, const Options& options, const std::regex& only, bool several,
                UsedFolders& used_folders, const Converter& converter, const PrepareArchive& prepare)
{
    const fs::path path = revosf2::GamePath(frame::PathFromUtf8(input));
    const std::string name = frame::Utf8(path.filename());
    ntdlib::disc::GameFiles game;
    if (const std::optional<std::string> error = ntdlib::disc::LoadGame(path, game, revosf2::DiscOptions()))
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), revosf2::GameMessage(*error).c_str());
        return 1;
    }

    const std::vector<std::string> archives = game.SoundArchives();
    if (archives.empty())
    {
        std::fprintf(stderr, "%s: the game holds no Wii sound archive (.brsar)\n", name.c_str());
        return 1;
    }

    const fs::path out_dir =
        options.output.empty() ? revosf2::GameOutputFolder(path) : frame::PathFromUtf8(options.output);
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
    const GameDol dol{game.dol, frame::Utf8(path)};
    int rc = 0;
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        // Separate the archives' reports with a blank line. Run separates the inputs' reports the same way.
        if (a > 0 && (options.list || !options.quiet))
        {
            std::printf("\n");
        }

        // The archive's path in the game names it. ConvertInput takes the archive's name from that path and writes to
        // the archive's folder. `open` reads the archive from the game.
        const std::string& archive_path = archives[a];
        const fs::path archive_input = path / frame::PathFromUtf8(archive_path);
        Options archive_options = options;
        archive_options.output = frame::Utf8(out_dir / frame::PathFromUtf8(seqlib::SafeFileName(folders[a])));
        Converter archive_converter = converter;
        archive_converter.open = [&game, &archive_path](const fs::path&,
                                                        const std::string& archive_name) -> std::optional<SoundArchive>
        {
            std::vector<uint8_t> bytes;
            if (!game.read_file(archive_path, bytes))
            {
                return std::nullopt;
            }

            return OpenGameArchive(std::move(bytes), archive_name);
        };
        archive_converter.prepare = [&prepare, &dol](const fs::path& in_game, const SoundArchive& archive,
                                                     const Options& given, PerformOptions&)
        {
            prepare(in_game, archive, given.quiet, &dol);
        };

        // An archive that fails doesn't stop the others.
        try
        {
            rc = std::max(rc, frame::ConvertInput(frame::Utf8(archive_input), archive_options, only,
                                                  several || archives.size() > 1, used_folders, archive_converter));
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", frame::Utf8(archive_input.filename()).c_str(), e.what());
            rc = 1;
        }
    }

    return rc;
}

// Processes the inputs on the command line and returns the exit status. frame::Run does the same for loose archives. A
// game converts each of its sound archives (ConvertGame). Any other input converts as a loose archive.
int Run(const std::vector<std::string>& args, const Converter& converter, const PrepareArchive& prepare)
{
    const Options options = frame::ParseOptions<PerformOptions>(args, converter.option);
    if (options.help)
    {
        converter.usage();
        return 0;
    }
    if (options.version)
    {
        std::puts(converter.version_line.c_str());
        return 0;
    }
    if (options.archives.empty())
    {
        converter.usage();
        return 2;
    }
    if (!options.output.empty() && options.archives.size() > 1)
    {
        throw std::runtime_error("--output requires a single " + converter.input_word +
                                 ". Otherwise, output goes in a folder beside each " + converter.input_word + ".");
    }

    converter.before_inputs();

    const std::regex only(options.only.empty() ? std::string(".*") : options.only);
    const bool several = options.archives.size() > 1;
    UsedFolders used_folders;
    int rc = 0;
    for (std::size_t i = 0; i < options.archives.size(); i++)
    {
        // Separate the inputs' reports with a blank line. The macOS app splits them there.
        if (i > 0 && (options.list || !options.quiet))
        {
            std::printf("\n");
        }

        // An input that fails doesn't stop the others.
        const std::string& input = options.archives[i];
        try
        {
            const int input_rc = revosf2::IsGameInput(frame::PathFromUtf8(input))
                                     ? ConvertGame(input, options, only, several, used_folders, converter, prepare)
                                     : frame::ConvertInput(input, options, only, several, used_folders, converter);
            rc = std::max(rc, input_rc);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", frame::Utf8(frame::PathFromUtf8(input).filename()).c_str(), e.what());
            rc = 1;
        }
    }

    return rc;
}

} // namespace

int main(int argc, char** argv)
{
    // revosf2's name, version line and the parts of its help that differ. ntdlib's UsageText writes the help.
    frame::ArchiveConverter<SoundArchive> program;
    program.program = "revosf2";
    program.version_line = "revosf2 " REVOSF2_VERSION;
    program.description =
        "revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont 2 and\n"
        "SFZ. An input can also be a game: a Wii disc image (ISO, NKit, WBFS or WDF),\n"
        "an extracted disc, or a WiiWare title's folder with its TMD and decrypted\n"
        "contents. Each sound archive of a game converts into a folder of its own.\n";
    program.extension = ".brsar";
    program.seed_option = "initial game random state (default 0x12345678)";
    program.extra_options =
        "  --dol FILE     the game's main.dol, for the build of the game's sound library\n"
        "                 (default: the game's main.dol when the input is a game, or\n"
        "                 sys/main.dol of the extracted disc that holds the archive)\n";

    // --dol names the game's main.dol. The build that it shows plays every archive.
    std::string dol;
    std::optional<SndBuild> dol_build;
    program.option = [&dol](const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
    {
        if (option == "--dol")
        {
            dol = reader.Value();
            return true;
        }

        return frame::VarOption(option, reader, perform);
    };

    // Each archive plays by the build that its game's main.dol shows, or else by the one its version gives.
    frame::Settings settings;
    const PrepareArchive prepare = [&dol, &dol_build, &settings](const fs::path& input, const SoundArchive& archive,
                                                                 bool quiet, const GameDol* game)
    {
        const std::optional<SndBuild> build = ArchiveBuild(input, archive, quiet, dol, dol_build, game);
        settings = SettingsFor(build.value_or(frame::BuildOfRsarVersion(archive.Version())));
    };

    // The frame's converter of a loose archive. frame::RunArchiveConverter makes the same converter.
    Converter converter;
    converter.program = program.program;
    converter.version_line = program.version_line;
    converter.input_word = "input";
    converter.folder_message = " is a folder, not a sound archive";
    converter.samples_folder = frame::kSfzSamplesFolder;
    converter.usage = [&program]
    {
        std::puts(frame::UsageText(program).c_str());
    };
    converter.option = frame::ArchiveOptions(program);
    converter.before_inputs = [&dol, &dol_build]
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
    converter.open = [&program](const fs::path& input, const std::string& name)
    {
        return frame::OpenArchive<SoundArchive>(input, name, program.settings, program.group_files);
    };
    converter.sequences = frame::ArchiveSequences<SoundArchive>;
    converter.prepare =
        [&prepare](const fs::path& input, const SoundArchive& archive, const Options& options, PerformOptions&)
    {
        prepare(input, archive, options.quiet, nullptr);
    };
    converter.convert = [&settings](SoundArchive& archive, std::size_t index, const PerformOptions& perform)
    {
        return frame::ConvertSequence(archive, static_cast<uint32_t>(index), perform, settings, "revosf2");
    };

    const auto run = [&converter, &prepare](const std::vector<std::string>& args)
    {
        return Run(args, converter, prepare);
    };

    return frame::RunMain(argc, argv, run);
}
