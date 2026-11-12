// SPDX-License-Identifier: MIT

// revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont and SFZ.
//
//     revosf2 [options] <archive.brsar> [<archive.brsar> ...]
//
// Output goes beside each archive. An input can also be a game: a Wii disc image, an extracted disc or a WiiWare
// title's folder. Each of its sound archives converts into a folder of its own. Those folders go in a folder beside the
// game. Music that a game plays as several sequences together converts as one piece (ntdlib/sound_pieces.h). revosf2
// also converts the music and sound effects of Wii games that play their sound with JAudio: from the game, or from the
// game's start-up archive (.baa or .aaf) with the game's other sound files beside it. ntdlib's converter of JAudio
// games converts them. Windows accepts files dropped on the executable; macOS uses Revosf2.app.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/dol.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/arrangements.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/jaudio/game.h"
#include "ntdlib/sequence_export/jaudio/game_input.h"
#include "ntdlib/sequence_export/jaudio/performance.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_pieces.h"
#include "seqlib/file_name.h"
#include "xsflib/rip/tool.h"

#include "games.h"
#include "profiles.h"
#include "program.h"
#include "snd_build.h"

namespace fs = std::filesystem;
namespace frame = ntdlib::sequence_export;
namespace jaudio = ntdlib::sequence_export::jaudio;
using ntdlib::FindMainDol;
using ntdlib::rsar::SoundArchive;
using ntdlib::sequence_export::PerformOptions;
using revosf2::Arrangements;
using revosf2::kProgram;
using revosf2::Profile;
using revosf2::ProfileOf;
using revosf2::SndBuild;
using revosf2::Weather;

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

// How revosf2 opens a JAudio game. The effects of the first JAudio convert for the games that ntdlib's rules list
// (jaudio::GameRules::known_effects), such as New Play Control! Pikmin and Pikmin 2.
constexpr jaudio::GameOptions kJAudioOptions{.known_effects = true};

// The words for the builds whose rules `build` plays by, in the note on an archive's build.
const char* BuildWords(const SndBuild& build)
{
    return build.October2006()          ? "builds of 2006"
           : build.SinceSeptember2009() ? "builds from September 2009"
           : build.SinceNovember2008()  ? "builds from November 2008"
           : build.SinceJune2008()      ? "builds from June 2008"
           : build.SinceMarch2008()     ? "builds from March 2008"
                                        : "builds of 2007";
}

// What a game's main.dol shows about a sound archive: the sound library that plays it (snd_build.h) and the game's
// profile.
struct GameCode
{
    std::optional<revosf2::SoundLibrary> library;
    Profile profile = Profile::kNone;
};

// What a game's main.dol, `bytes`, shows. A main.dol without AX microcode shows neither the library nor the profile.
GameCode CodeOf(std::span<const uint8_t> bytes)
{
    GameCode code;
    code.library = revosf2::ReadSoundLibrary(bytes);
    code.profile = code.library ? ProfileOf(bytes) : Profile::kNone;

    return code;
}

// A main.dol and what it shows, with the path that the messages give it: --dol's file, the path of a game given as the
// input, or the main.dol of the extracted disc that holds an archive. `warning` is the warning about a main.dol that
// was found but shows no build, or else empty.
struct DolCode
{
    std::string path;
    GameCode code;
    std::string warning;
};

// The main.dol that shows how to play `input`'s archive when --dol gives none: `game`, the main.dol of a game given as
// the input, or else the main.dol of the extracted disc that holds the archive. A main.dol that shows no build gets a
// warning. The path is empty if there's no main.dol.
DolCode FoundDol(const fs::path& input, const DolCode* game)
{
    const std::string name = frame::Utf8(input.filename());
    if (game)
    {
        DolCode found = *game;
        if (!found.code.library)
        {
            found.warning =
                "the main.dol of " + found.path + " holds no AX microcode: " + name + " plays as its version shows";
        }

        return found;
    }

    const std::optional<fs::path> path = FindMainDol(input);
    if (!path)
    {
        return {};
    }

    DolCode found{frame::Utf8(*path), {}, {}};
    const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(*path);
    if (!bytes)
    {
        found.warning = "can't read " + found.path + ": " + name + " plays as its version shows";
        return found;
    }

    found.code = CodeOf(*bytes);
    if (!found.code.library)
    {
        found.warning = found.path + " holds no AX microcode: " + name + " plays as its version shows";
    }

    return found;
}

// The main.dol that shows how to play the archive at `input`: `given`, from --dol, or else the one that FoundDol finds.
// `game` is the main.dol of the game that holds the archive, if the input was a game.
DolCode ArchiveDol(const fs::path& input, const DolCode& given, const DolCode* game)
{
    return given.code.library ? given : FoundDol(input, game);
}

// Prints the warning about `dol`, the main.dol that shows how to play `archive` from `input`. Unless `quiet` is set,
// also notes which build plays the archive. Without a main.dol, the archive's version gives the build.
void NoteDol(const fs::path& input, const SoundArchive& archive, bool quiet, const DolCode& dol)
{
    if (!dol.warning.empty())
    {
        std::fprintf(stderr, "warning: %s\n", dol.warning.c_str());
    }

    if (quiet)
    {
        return;
    }

    // A main.dol that was found but can't give the build has had its warning.
    const std::string name = frame::Utf8(input.filename());
    if (dol.code.library)
    {
        std::fprintf(stderr, "note: %s plays as NW4R SND's %s do, by the game's main.dol (%s)\n", name.c_str(),
                     BuildWords(dol.code.library->build), dol.path.c_str());
    }
    else if (dol.path.empty() && archive.Version() < 0x0104)
    {
        std::fprintf(stderr,
                     "note: no main.dol found for %s: it plays as NW4R SND's builds of 2007 do (--dol gives the game's "
                     "main.dol)\n",
                     name.c_str());
    }
}

// The archive that the frame converts: the main.dol that shows how to play it, and its pieces of several sequences.
// `open` finds the main.dol as the archive opens. `sequences` finds the pieces. The frame lists an archive's sequences
// before it prepares the archive.
struct OpenedArchive
{
    DolCode dol;
    std::vector<ntdlib::SoundPiece> pieces;
};

// What the command line chooses, and the archive that the frame converts. --dol names the game's main.dol
// (`dol.path`). `before_inputs` reads the file into `dol.code`. The build that this main.dol shows plays every archive.
// --weather and --museum choose the arrangements of City Folk's music. `open` sets `opened` for each archive. `prepare`
// sets `settings`.
struct State
{
    DolCode dol;
    Arrangements arrangements;
    bool chose_arrangements = false;
    OpenedArchive opened;
    frame::Settings settings;
};

// The variants of each sound of `archive` in the game's profile (profiles.h), as arrangements, in the order of
// VariantsOf. A variant plays its tracks in place of those that SetUpSound gives. `archive` has to outlive the result.
frame::ArrangementsOf ArchiveVariants(const State& state, const SoundArchive& archive)
{
    return [variants = revosf2::VariantsOf(state.opened.dol.code.profile, state.chose_arrangements),
            &archive](std::size_t sound)
    {
        std::vector<frame::Arrangement> arrangements;
        for (const revosf2::Variant& variant : variants)
        {
            if (variant.sound == archive.Sounds()[sound].name)
            {
                arrangements.push_back(
                    {variant.suffix, [tracks = variant.tracks](PerformOptions& options) { options.tracks = tracks; }});
            }
        }

        return arrangements;
    };
}

// The words for `weather` in the note on City Folk's music.
const char* WeatherWords(Weather weather)
{
    switch (weather)
    {
    case Weather::kFine:
        return "fine weather";
    case Weather::kRain:
        return "rain";
    case Weather::kSnow:
        return "snow";
    }

    return "";
}

// What a profile changes in the sounds of an archive, for the note on the profile.
struct ProfileChanges
{
    bool tracks = false;    // it silences tracks of a sound
    bool variables = false; // it sets a variable of a sound, at the start or later
    bool sends = false;     // it changes a sound's sends to the aux buses
    bool variants = false;  // it converts a sound once more in a variant
};

// What `profile` changes in the sounds of `archive`, in the arrangements that the game starts its music in.
ProfileChanges ChangesOf(Profile profile, const SoundArchive& archive)
{
    ProfileChanges changes;
    for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
    {
        PerformOptions options;
        revosf2::SetUpSound(profile, {}, sound.name, options);
        changes.tracks = changes.tracks || options.tracks != PerformOptions{}.tracks;
        changes.variables = changes.variables || !options.variables.empty() || !options.changes.empty();
        changes.sends = changes.sends || options.fx_sends != PerformOptions{}.fx_sends;
    }

    for (const revosf2::Variant& variant : revosf2::VariantsOf(profile, false))
    {
        changes.variants = changes.variants || archive.FindSound(std::string(variant.sound)).has_value();
    }

    return changes;
}

// Notes how `archive`, named `name`, plays its music by `profile`, in `arrangements`. The profiles of Wii Play, Wii
// Music, Wii Sports, Mario Kart Wii, City Folk and Smooth Moves get a note only for an archive whose sounds they
// change. The channels' profiles change bus B for every sound. `chosen` is set when --weather or --museum gives the
// arrangements. They change nothing in an archive without City Folk's profile. The note says so.
void NoteProfile(const std::string& name, const SoundArchive& archive, Profile profile,
                 const Arrangements& arrangements, bool chosen)
{
    const ProfileChanges changes = ChangesOf(profile, archive);
    if (profile == Profile::kWiiPlay && changes.tracks)
    {
        const char* states = changes.variants
                                 ? " Its piece converts as the minigame starts it, and once more for each later set "
                                   "of tracks."
                                 : "";
        std::fprintf(stderr,
                     "note: %s plays its music as Wii Play's minigames play it.%s Tracks that the game mutes get CC11 "
                     "0.\n",
                     name.c_str(), states);
    }

    if (profile == Profile::kWiiMusic && changes.tracks)
    {
        std::fprintf(stderr,
                     "note: %s plays its main menu's music and Pitch Perfect's beats as Wii Music starts them, "
                     "and once more in each of their other states. Tracks that the game silences get CC11 0. CC91 "
                     "adds the game's send to Pitch Perfect's phrases and the jingles of its pictures.\n",
                     name.c_str());
    }

    if (profile == Profile::kMiiChannel)
    {
        std::fprintf(stderr,
                     "note: %s plays its music as the Mii Channel starts it, and once more in each of three other "
                     "sets of tracks. Tracks that the channel silences get CC11 0. CC91 adds the channel's send to its "
                     "other sounds. CC93 is 0: the channel puts no effect on bus B.\n",
                     name.c_str());
    }

    if (profile == Profile::kWiiShopChannel)
    {
        std::fprintf(stderr,
                     "note: %s plays as the Wii Shop Channel plays it. CC93 is 0: the channel puts no effect "
                     "on bus B.\n",
                     name.c_str());
    }

    if (profile == Profile::kWiiSports && changes.variables)
    {
        std::fprintf(stderr,
                     "note: %s plays golf's wind, SE_LV_Wind, at a speed of 8 and in direction 0. Wii Sports sets "
                     "both by the hole. --var 0=N gives another speed (1 to 15), and --var 1=N another direction "
                     "(0 to 7).\n",
                     name.c_str());
    }

    if (profile == Profile::kMarioKart && changes.tracks)
    {
        std::fprintf(stderr,
                     "note: %s plays the music of the menus and of the Wi-Fi menus as Mario Kart Wii starts it, and "
                     "once more in each of its other states. Tracks that the game silences get CC11 0. Six sound "
                     "effects play variable 0 at the value of their first play in the game.\n",
                     name.c_str());
    }

    if (profile == Profile::kCityFolk && (changes.tracks || changes.variables || changes.sends))
    {
        const char* others = chosen ? "--weather and --museum choose the others."
                                    : "The music converts once more in rain and in snow, and the museum's music in "
                                      "arrangements 2 to 6. --weather and --museum choose one of them instead.";
        std::fprintf(stderr,
                     "note: %s plays its music as Animal Crossing: City Folk starts it in %s. The museum's music plays "
                     "arrangement %d of %d. %s Tracks that the game silences get CC11 0. CC91 adds the game's sends "
                     "to its reverb.\n",
                     name.c_str(), WeatherWords(arrangements.weather), arrangements.museum,
                     revosf2::kMuseumArrangements, others);
    }
    else if (profile == Profile::kSmoothMoves && changes.sends)
    {
        std::fprintf(stderr,
                     "note: %s plays the music of WarioWare: Smooth Moves' microgames with the reverb of the stage "
                     "that plays each piece first. The pieces of the stages without a reverb get CC91 0.\n",
                     name.c_str());
    }

    if (profile != Profile::kCityFolk && chosen)
    {
        std::fprintf(stderr,
                     "note: --weather and --museum change nothing in %s. They apply only with the main.dol of "
                     "Animal Crossing: City Folk (USA).\n",
                     name.c_str());
    }
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
    std::optional<jaudio::Game> opened = jaudio::Game::FromDiscFiles(std::move(game), kProgram, kJAudioOptions);
    if (!opened)
    {
        std::fprintf(stderr,
                     "%s: the game holds no Wii sound archive (.brsar) and no JAudio start-up archive (.baa or .aaf)\n",
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
// name them. The game's main.dol gives the build unless --dol gives one (`state.dol`). A game without a sound archive
// converts as a JAudio game (ConvertJAudioGame). `several` is set when the command line has more than one input. `open`
// sets `state.opened` for each archive. Returns 1 on any failure, otherwise 0.
int ConvertGame(const std::string& input, const Options& options, const std::regex& only, bool several,
                UsedFolders& used_folders, const Converter& converter, State& state, const JAudioInputs& jaudio_inputs)
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
    const DolCode dol{frame::Utf8(path), CodeOf(game->dol), {}};
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
        archive_converter.open = [&game, &archive_path, &archive_input, &state, &dol](
                                     const fs::path&, const std::string& archive_name) -> std::optional<SoundArchive>
        {
            std::vector<uint8_t> bytes;
            if (!game->read_file(archive_path, bytes))
            {
                return std::nullopt;
            }

            state.opened = {ArchiveDol(archive_input, state.dol, &dol), {}};
            return frame::LoadArchive<SoundArchive>(std::move(bytes), archive_name, frame::Settings());
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
// beside it (jaudio::ArchiveProgramGames). Any other input converts as a loose archive.
int Run(const std::vector<std::string>& args, const Converter& converter, State& state,
        const jaudio::GameConverter& games)
{
    // A JAudio game takes the same options. None of the options that revosf2 adds applies to its sequences: it leaves
    // out --dol, --var, --weather and --museum (jaudio::ArchiveProgramOptions).
    const JAudioInputs jaudio_inputs{games, frame::ParseOptions<jaudio::PerformOptions>(args, games.option)};
    const frame::OtherInput<PerformOptions> archive_games = jaudio::ArchiveProgramGames(args, games);
    const frame::OtherInput<PerformOptions> other = [&converter, &state, &jaudio_inputs, &archive_games](
                                                        const std::string& input, const Options& options,
                                                        const std::regex& only, bool several,
                                                        UsedFolders& used_folders) -> std::optional<int>
    {
        if (revosf2::IsGameInput(frame::PathFromUtf8(input)))
        {
            return ConvertGame(input, options, only, several, used_folders, converter, state, jaudio_inputs);
        }

        return archive_games(input, options, only, several, used_folders);
    };
    return frame::Run(args, converter, other);
}

// The program name, version line and the parts of the help that differ for revosf2. UsageText in ntdlib writes the
// help. The option hook sets `state`. `state` has to outlive the result.
frame::ArchiveConverter<SoundArchive> Program(State& state)
{
    frame::ArchiveConverter<SoundArchive> program;
    program.program = kProgram;
    program.version_line = std::string(kProgram) + " " REVOSF2_VERSION;
    program.description =
        "revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont 2 and\n"
        "SFZ. An input can also be a game: a Wii disc image (ISO, NKit, WBFS or WDF),\n"
        "an extracted disc, or a WiiWare title's folder with its TMD and decrypted\n"
        "contents. Each sound archive of a game converts into a folder of its own.\n"
        "It also converts the music and sound effects of Wii games that use JAudio:\n"
        "give the game, or the game's start-up archive (.baa or .aaf) with the Seqs\n"
        "folder and the Waves or Banks folder beside it. Pikmin has no start-up\n"
        "archive. Give the game.\n";
    program.extension = ".brsar";
    program.seed_option =
        "initial game random state (default 0x12345678, or 0 for\n"
        "                 a JAudio game)";
    program.extra_options =
        "  --dol FILE     the game's main.dol, for the build of the game's sound library\n"
        "                 (default: the game's main.dol when the input is a game, or\n"
        "                 sys/main.dol of the extracted disc that holds the archive)\n"
        "  --weather W    convert Animal Crossing: City Folk's hourly and town music in\n"
        "                 one weather: fine, rain or snow (default: all three)\n"
        "  --museum N     convert City Folk's museum music in one arrangement, from 1\n"
        "                 to 7 (default: 1 to 6)\n";

    program.option = [&state](const std::string& option, frame::OptionReader& reader, PerformOptions& perform)
    {
        if (option == "--dol")
        {
            state.dol.path = reader.Value();
            return true;
        }

        if (option == "--weather")
        {
            const std::string value = reader.Value();
            const std::optional<Weather> weather = revosf2::WeatherNamed(value);
            if (!weather)
            {
                throw std::runtime_error("--weather requires fine, rain or snow, not " + value);
            }

            state.arrangements.weather = *weather;
            state.chose_arrangements = true;
            return true;
        }

        if (option == "--museum")
        {
            state.arrangements.museum = static_cast<int>(reader.Number(revosf2::kMuseumArrangements));
            if (state.arrangements.museum == 0)
            {
                throw std::runtime_error("--museum 0 is out of range");
            }

            state.chose_arrangements = true;
            return true;
        }

        return frame::VarOption(option, reader, perform);
    };

    return program;
}

// Converts sound `index` of `archive`, set up as the game's code sets it up (SetUpSound). An index past the archive's
// sounds is a variant (frame::WithArrangements). A piece of several sequences converts as one (ntdlib/sound_pieces.h).
frame::Conversion ConvertSound(const State& state, const SoundArchive& archive, std::size_t index,
                               const PerformOptions& perform)
{
    const frame::ArrangedSound at = frame::ArrangedSoundAt(index, archive.Sounds().size());
    PerformOptions options = perform;
    revosf2::SetUpSound(state.opened.dol.code.profile, state.arrangements, archive.Sounds()[at.sound].name, options);
    frame::SetUpArrangement(at, ArchiveVariants(state, archive), options);
    const ntdlib::SoundPiece* piece = at.arrangement ? nullptr : ntdlib::PieceStartingAt(state.opened.pieces, at.sound);
    if (piece)
    {
        return frame::ConvertTogether(archive, piece->sounds, piece->name, options, state.settings, kProgram);
    }

    return frame::ConvertSequence(archive, static_cast<uint32_t>(at.sound), options, state.settings, kProgram);
}

// The frame's converter of a loose archive, from frame::MakeArchiveConverter. An input can be a game too. `open` sets
// `state.opened` for each loose archive, with the main.dol that --dol gives or FoundDol finds. ConvertGame sets it for
// a game's archives. `program` and `state` have to outlive the converter.
Converter MakeConverter(const frame::ArchiveConverter<SoundArchive>& program, State& state)
{
    Converter converter = frame::MakeArchiveConverter(program);
    converter.input_word = "input";
    const auto open_archive = converter.open;
    converter.open = [open_archive, &state](const fs::path& input, const std::string& name)
    {
        std::optional<SoundArchive> archive = open_archive(input, name);
        if (archive)
        {
            state.opened = {ArchiveDol(input, state.dol, nullptr), {}};
        }

        return archive;
    };

    // --dol's main.dol is read before the inputs.
    converter.before_inputs = [&state]
    {
        DolCode& dol = state.dol;
        if (!dol.path.empty())
        {
            const std::optional<std::vector<uint8_t>> bytes = frame::ReadFile(frame::PathFromUtf8(dol.path));
            dol.code = bytes ? CodeOf(*bytes) : GameCode{};
            if (!dol.code.library)
            {
                throw std::runtime_error(bytes ? dol.path + " holds no AX microcode: it isn't a game's main.dol"
                                               : "can't read " + dol.path);
            }
        }
    };

    // A piece of several sequences converts as one (ntdlib/sound_pieces.h). A sound with variants converts once more
    // for each (profiles.h). This hook finds the archive's pieces. The frame runs it once for each archive. --only
    // picks a variant with its sound.
    converter.sequences = [&state](const SoundArchive& archive)
    {
        state.opened.pieces = ntdlib::FindSoundPieces(archive, ntdlib::kMegaMan9Channels);
        return frame::WithArrangements(
            frame::ArchiveSequences(archive, std::span<const ntdlib::SoundPiece>(state.opened.pieces)),
            archive.Sounds().size(), ArchiveVariants(state, archive), true);
    };

    // Each archive plays by the build that the game's main.dol shows, or else by the build that the archive's version
    // gives.
    converter.prepare =
        [&state](const fs::path& input, const SoundArchive& archive, const Options& options, PerformOptions&)
    {
        NoteDol(input, archive, options.quiet, state.opened.dol);
        if (!options.quiet)
        {
            NoteProfile(frame::Utf8(input.filename()), archive, state.opened.dol.code.profile, state.arrangements,
                        state.chose_arrangements);
        }

        state.settings = revosf2::ArchiveSettings(state.opened.dol.code.library, archive.Version());
    };

    converter.convert = [&state](SoundArchive& archive, std::size_t index, const PerformOptions& perform)
    {
        return ConvertSound(state, archive, index, perform);
    };

    return converter;
}

} // namespace

int main(int argc, char** argv)
{
    State state;
    const frame::ArchiveConverter<SoundArchive> program = Program(state);
    const Converter converter = MakeConverter(program, state);

    // The frame's converter of JAudio games, for a Wii game that plays its music with JAudio.
    const jaudio::GameConverter games =
        jaudio::MakeGameConverter(program.program, program.version_line,
                                  jaudio::ArchiveProgramOptions(converter.option), converter.usage, kJAudioOptions);

    const auto run = [&converter, &state, &games](const std::vector<std::string>& args)
    {
        return Run(args, converter, state, games);
    };
    return frame::RunMain(argc, argv, run);
}
