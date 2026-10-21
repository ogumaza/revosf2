// SPDX-License-Identifier: MIT

// revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont and SFZ.
//
//     revosf2 [options] <archive.brsar> [<archive.brsar> ...]
//
// Output goes beside each archive. An input can also be a game: a Wii disc image, an extracted disc or a WiiWare
// title's folder. Each of its sound archives converts into a folder of its own. Those folders go in a folder beside the
// game. Music that a game plays as several sequences together converts as one piece (pieces.h). revosf2 also converts
// the music and sound effects of Wii games that play their sound with JAudio: from the game, or from the game's
// start-up archive (.baa) with the game's other sound files beside it. ntdlib's converter of JAudio games converts
// them. Windows accepts files dropped on the executable; macOS uses Revosf2.app.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
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
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "seqlib/file_name.h"
#include "xsflib/rip/tool.h"

#include "games.h"
#include "pieces.h"
#include "profiles.h"
#include "snd_build.h"

namespace fs = std::filesystem;
namespace frame = ntdlib::sequence_export;
namespace jaudio = ntdlib::sequence_export::jaudio;
using ntdlib::rsar::SoundArchive;
using ntdlib::sequence_export::PerformOptions;
using revosf2::FindMainDol;
using revosf2::Profile;
using revosf2::ProfileOf;
using revosf2::ReadSndBuild;
using revosf2::SndBuild;

namespace
{

using Converter = frame::Converter<PerformOptions, SoundArchive, frame::Conversion>;
using Options = frame::Options<PerformOptions>;
using UsedFolders = std::vector<std::pair<fs::path, std::string>>;

// The JAudio games among the inputs: the frame's converter of JAudio games, and the options for them.
struct JAudioInputs
{
    const jaudio::GameConverter& converter;
    frame::Options<jaudio::PerformOptions> options;
};

// The rules that `build` plays by.
const char* RulesOf(const SndBuild& build)
{
    return build.October2006()          ? "builds of 2006"
           : build.SinceSeptember2009() ? "builds from September 2009"
           : build.SinceNovember2008()  ? "builds from November 2008"
           : build.SinceJune2008()      ? "builds from June 2008"
           : build.SinceMarch2008()     ? "builds from March 2008"
                                        : "builds of 2007";
}

// What a game's main.dol shows about a sound archive: the build of NW4R SND that plays it, and the game's profile.
struct GameCode
{
    std::optional<SndBuild> build;
    Profile profile = Profile::kNone;
};

// What a game's main.dol, `bytes`, shows. A main.dol without AX microcode shows neither the build nor the profile.
GameCode CodeOf(std::span<const uint8_t> bytes)
{
    GameCode code;
    code.build = ReadSndBuild(bytes);
    code.profile = code.build ? ProfileOf(bytes) : Profile::kNone;

    return code;
}

// A main.dol and what it shows, with the path that the messages give it: --dol's file, the path of a game given as the
// input, or the main.dol of the extracted disc that holds an archive.
struct DolCode
{
    std::string path;
    GameCode code;
};

// Sets up the conversion of the sequences of `archive`, read from `input`, and prints which build plays it unless
// `quiet` is set. `game` is the main.dol of the game that holds the archive, if the input was a game.
using PrepareArchive =
    std::function<void(const fs::path& input, const SoundArchive& archive, bool quiet, const DolCode* game)>;

// The main.dol that shows how to play `input`'s archive when --dol gives none: `game`, the main.dol of a game given as
// the input, or else the main.dol of the extracted disc that holds the archive. Warn about a main.dol that shows no
// build. The path is empty if there's no main.dol.
DolCode FoundDol(const fs::path& input, const DolCode* game)
{
    const std::string name = frame::Utf8(input.filename());
    if (game)
    {
        if (!game->code.build)
        {
            std::fprintf(stderr, "warning: the main.dol of %s holds no AX microcode: %s plays as its version shows\n",
                         game->path.c_str(), name.c_str());
        }

        return *game;
    }

    const std::optional<fs::path> path = FindMainDol(input);
    if (!path)
    {
        return {};
    }

    DolCode found{frame::Utf8(*path), {}};
    const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(*path);
    if (!bytes)
    {
        std::fprintf(stderr, "warning: can't read %s: %s plays as its version shows\n", found.path.c_str(),
                     name.c_str());
        return found;
    }

    found.code = CodeOf(*bytes);
    if (!found.code.build)
    {
        std::fprintf(stderr, "warning: %s holds no AX microcode: %s plays as its version shows\n", found.path.c_str(),
                     name.c_str());
    }

    return found;
}

// What the game's main.dol shows about `archive`, from `input`. That main.dol is `given`, from --dol, or else the one
// that FoundDol finds. Without one, the archive's version gives the build. Say which unless `quiet` is set.
GameCode ArchiveCode(const fs::path& input, const SoundArchive& archive, bool quiet, const DolCode& given,
                     const DolCode* game)
{
    const DolCode dol = given.code.build ? given : FoundDol(input, game);
    if (quiet)
    {
        return dol.code;
    }

    // A main.dol that was found but can't give the build has had its warning.
    const std::string name = frame::Utf8(input.filename());
    if (dol.code.build)
    {
        std::fprintf(stderr, "note: %s plays as NW4R SND's %s do, by the game's main.dol (%s)\n", name.c_str(),
                     RulesOf(*dol.code.build), dol.path.c_str());
        if (dol.code.profile == Profile::kCityFolk)
        {
            std::fprintf(stderr,
                         "note: %s plays its music as Animal Crossing: City Folk starts it in fine weather. Tracks "
                         "that the game silences get CC11 0. CC91 adds the game's sends to its reverb.\n",
                         name.c_str());
        }
    }
    else if (dol.path.empty() && archive.Version() < 0x0104)
    {
        std::fprintf(stderr,
                     "note: no main.dol found for %s: it plays as NW4R SND's builds of 2007 do (--dol gives the game's "
                     "main.dol)\n",
                     name.c_str());
    }

    return dol.code;
}

// The folder that an archive of the game at `game` writes to: `folder` in --output's folder (`output`), or else in the
// game's output folder. xsflib's ArchiveFolders gives `folder` the name that the rip tools give it.
std::string ArchiveOutput(const fs::path& game, const std::string& output, const std::string& folder)
{
    const fs::path out_dir = output.empty() ? revosf2::GameOutputFolder(game) : frame::PathFromUtf8(output);
    return frame::Utf8(out_dir / frame::PathFromUtf8(seqlib::SafeFileName(folder)));
}

// Converts the JAudio game at `path`, whose files `game` holds, the way ConvertGame converts a sound archive: into a
// folder of its own in the game's output folder, named after the game's start-up archive. Returns 1 if the game has no
// start-up archive or on any failure, otherwise 0.
int ConvertJAudioGame(const fs::path& path, std::shared_ptr<ntdlib::disc::GameFiles> game, const std::regex& only,
                      bool several, UsedFolders& used_folders, const JAudioInputs& jaudio_inputs)
{
    std::optional<jaudio::Game> opened = jaudio::Game::FromDiscFiles(std::move(game), "revosf2");
    if (!opened)
    {
        std::fprintf(stderr, "%s: the game holds no Wii sound archive (.brsar) and no JAudio start-up archive (.baa)\n",
                     frame::Utf8(path.filename()).c_str());
        return 1;
    }

    const std::string archive_path = opened->DiscPath();
    frame::Options<jaudio::PerformOptions> options = jaudio_inputs.options;
    options.output = ArchiveOutput(path, options.output, xsflib::rip::ArchiveFolders({archive_path})[0]);
    const jaudio::GameConverter converter =
        jaudio::MakeOpenGameConverter(jaudio_inputs.converter, [&opened] { return std::move(opened); });

    return frame::ConvertInput(frame::Utf8(path / frame::PathFromUtf8(archive_path)), options, only, several,
                               used_folders, converter);
}

// Converts each sound archive of the game at `input` the way frame::ConvertInput converts a loose archive. Each goes in
// a folder of its own in the game's output folder. xsflib's ArchiveFolders names those folders the way the rip tools
// name them. The game's main.dol gives the build unless --dol gives one (`prepare`). A game without a sound archive
// converts as a JAudio game (ConvertJAudioGame). `several` is set when the command line has more than one input.
// Returns 1 on any failure, otherwise 0.
int ConvertGame(const std::string& input, const Options& options, const std::regex& only, bool several,
                UsedFolders& used_folders, const Converter& converter, const PrepareArchive& prepare,
                const JAudioInputs& jaudio_inputs)
{
    const fs::path path = revosf2::GamePath(frame::PathFromUtf8(input));
    const std::string name = frame::Utf8(path.filename());
    const auto game = std::make_shared<ntdlib::disc::GameFiles>();
    if (const std::optional<std::string> error = ntdlib::disc::LoadGame(path, *game, revosf2::DiscOptions()))
    {
        std::fprintf(stderr, "%s: %s\n", name.c_str(), revosf2::GameMessage(*error).c_str());
        return 1;
    }

    const std::vector<std::string> archives = game->SoundArchives();
    if (archives.empty())
    {
        return ConvertJAudioGame(path, game, only, several, used_folders, jaudio_inputs);
    }

    // Each archive gets a folder of its own. The game's main.dol shows the archives' build unless --dol gives one.
    const std::vector<std::string> folders = xsflib::rip::ArchiveFolders(archives);
    const DolCode dol{frame::Utf8(path), CodeOf(game->dol)};
    int rc = 0;
    for (std::size_t a = 0; a < archives.size(); a++)
    {
        // Separate the archives' reports with a blank line. Run separates the inputs' reports the same way.
        if (a > 0 && (options.list || !options.quiet))
        {
            std::printf("\n");
        }

        // The archive's path in the game names it. ConvertInput takes the archive's name from that path and writes to
        // the archive's folder.
        const std::string& archive_path = archives[a];
        const fs::path archive_input = path / frame::PathFromUtf8(archive_path);
        Options archive_options = options;
        archive_options.output = ArchiveOutput(path, options.output, folders[a]);

        // `open` reads the archive from the game, with the warnings of an archive's file. An RSAR archive takes none of
        // the settings' read options.
        Converter archive_converter = converter;
        archive_converter.open = [&game, &archive_path](const fs::path&,
                                                        const std::string& archive_name) -> std::optional<SoundArchive>
        {
            std::vector<uint8_t> bytes;
            if (!game->read_file(archive_path, bytes))
            {
                return std::nullopt;
            }

            return frame::LoadArchive<SoundArchive>(std::move(bytes), archive_name, frame::Settings());
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

// Processes the inputs on the command line with the frame's Run, and returns the exit status. A game converts each of
// its sound archives (ConvertGame). A JAudio game's start-up archive converts with `games`, with the game's other files
// beside it. Any other input converts as a loose archive. jaudio::RunArchiveConverterWithGames takes JAudio games the
// same way.
int Run(const std::vector<std::string>& args, const Converter& converter, const PrepareArchive& prepare,
        const jaudio::GameConverter& games)
{
    // A JAudio game takes the same options. It leaves out --dol and --var (jaudio::ArchiveProgramOptions).
    const JAudioInputs jaudio_inputs{games, frame::ParseOptions<jaudio::PerformOptions>(args, games.option)};
    const frame::OtherInput<PerformOptions> other = [&converter, &prepare, &games, &jaudio_inputs](
                                                        const std::string& input, const Options& options,
                                                        const std::regex& only, bool several,
                                                        UsedFolders& used_folders) -> std::optional<int>
    {
        const fs::path path = frame::PathFromUtf8(input);
        if (revosf2::IsGameInput(path))
        {
            return ConvertGame(input, options, only, several, used_folders, converter, prepare, jaudio_inputs);
        }
        if (jaudio::IsStartUpArchiveFile(path))
        {
            return frame::ConvertInput(input, jaudio_inputs.options, only, several, used_folders, games);
        }

        return std::nullopt;
    };
    return frame::Run(args, converter, other);
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
        "contents. Each sound archive of a game converts into a folder of its own.\n"
        "It also converts the music and sound effects of Wii games that use JAudio:\n"
        "give the game, or the game's start-up archive (.baa) with the Seqs and Waves\n"
        "folders beside it.\n";
    program.extension = ".brsar";
    program.seed_option = "initial game random state (default 0x12345678)";
    program.extra_options =
        "  --dol FILE     the game's main.dol, for the build of the game's sound library\n"
        "                 (default: the game's main.dol when the input is a game, or\n"
        "                 sys/main.dol of the extracted disc that holds the archive)\n";

    // --dol names the game's main.dol. The build that it shows plays every archive.
    DolCode dol;
    program.option = [&dol](const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
    {
        if (option == "--dol")
        {
            dol.path = reader.Value();
            return true;
        }

        return frame::VarOption(option, reader, perform);
    };

    // Each archive plays by the build that its game's main.dol shows, or else by the one its version gives. A game with
    // a profile sets up the sounds that its code sets up as it starts them. The archive's pieces of several sequences
    // are found once, before its first conversion.
    frame::Settings settings;
    Profile profile = Profile::kNone;
    std::vector<revosf2::Piece> pieces;
    const PrepareArchive prepare = [&dol, &settings, &profile, &pieces](const fs::path& input,
                                                                        const SoundArchive& archive, bool quiet,
                                                                        const DolCode* game)
    {
        const GameCode code = ArchiveCode(input, archive, quiet, dol, game);
        settings = frame::RevolutionSettings(code.build.value_or(frame::BuildOfRsarVersion(archive.Version())));
        profile = code.profile;
        pieces = revosf2::FindPieces(archive);
    };

    // The frame's converter of a loose archive, from frame::MakeArchiveConverter. An input can be a game too.
    Converter converter = frame::MakeArchiveConverter(program);
    converter.input_word = "input";

    // --dol's main.dol is read before the inputs.
    converter.before_inputs = [&dol]
    {
        if (!dol.path.empty())
        {
            const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(frame::PathFromUtf8(dol.path));
            dol.code = bytes ? CodeOf(*bytes) : GameCode{};
            if (!dol.code.build)
            {
                throw std::runtime_error(bytes ? dol.path + " holds no AX microcode: it isn't a game's main.dol"
                                               : "can't read " + dol.path);
            }
        }
    };

    // Each archive plays by its build and profile.
    converter.prepare =
        [&prepare](const fs::path& input, const SoundArchive& archive, const Options& options, PerformOptions&)
    {
        prepare(input, archive, options.quiet, nullptr);
    };

    // A piece of several sequences converts as one (pieces.h).
    converter.sequences = revosf2::ArchiveSequences;
    converter.convert =
        [&settings, &profile, &pieces](SoundArchive& archive, std::size_t index, const PerformOptions& perform)
    {
        PerformOptions options = perform;
        revosf2::SetUpSound(profile, archive.Sounds()[index].name, options);
        if (const std::optional<revosf2::Piece> piece = revosf2::PieceAt(pieces, index))
        {
            return frame::ConvertTogether(archive, piece->sounds, piece->name, options, settings, "revosf2");
        }

        return frame::ConvertSequence(archive, static_cast<uint32_t>(index), options, settings, "revosf2");
    };

    // The frame's converter of JAudio games, for a Wii game that plays its music with JAudio.
    const jaudio::GameConverter games = jaudio::MakeGameConverter(
        program.program, program.version_line, jaudio::ArchiveProgramOptions(converter.option), converter.usage);

    const auto run = [&converter, &prepare, &games](const std::vector<std::string>& args)
    {
        return Run(args, converter, prepare, games);
    };
    return frame::RunMain(argc, argv, run);
}
