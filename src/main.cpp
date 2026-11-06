// SPDX-License-Identifier: MIT

// revosf2: convert Wii sound archive sequences (.brsar) to MIDI, SoundFont and SFZ.
//
//     revosf2 [options] <archive.brsar> [<archive.brsar> ...]
//
// Output goes beside each archive. An input can also be a game: a Wii disc image, an extracted disc or a WiiWare
// title's folder. Each of its sound archives converts into a folder of its own. Those folders go in a folder beside the
// game. Music that a game plays as several sequences together converts as one piece (pieces.h). revosf2 also converts
// the music and sound effects of Wii games that play their sound with JAudio: from the game, or from the game's
// start-up archive (.baa or .aaf) with the game's other sound files beside it. ntdlib's converter of JAudio games
// converts them. Windows accepts files dropped on the executable; macOS uses Revosf2.app.

#include <algorithm>
#include <array>
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
#include <string_view>
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
using revosf2::Arrangements;
using revosf2::FindMainDol;
using revosf2::Profile;
using revosf2::ProfileOf;
using revosf2::ReadAxVoices;
using revosf2::ReadSndBuild;
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

// The games of the first JAudio whose sound effects revosf2 converts, by the SHA-1s of their start-up archives
// (jaudio::GameOptions::jaudio1_effects). Pikmin has no start-up archive, and its bank file takes the archive's place.
// Each of these games plays its effects in the effects sequence that ntdlib's rules for the game name. revosf2 leaves
// out the effects of the other games of the first JAudio.
constexpr std::array<std::string_view, 2> kEffectsGames{
    "9f42f65be7f7159faa14b003524476d624e1227f", // New Play Control! Pikmin's bank file
    "6747c3c7bce964bc62f817e1c8c32711c905c4c3", // New Play Control! Pikmin 2
};

// Whether `game` is one of kEffectsGames.
bool ListsEffects(const jaudio::Game& game)
{
    return std::ranges::find(kEffectsGames, game.ArchiveSha1()) != kEffectsGames.end();
}

// The JAudio game whose start-up archive is at `path`, with the game's other files beside it. A game of kEffectsGames
// opens a second time, with its effects. Throws jaudio::FormatError if the archive can't be read.
jaudio::Game OpenJAudioArchive(const fs::path& path)
{
    jaudio::Game game = jaudio::Game::FromFolder(path, "revosf2");
    return ListsEffects(game) ? jaudio::Game::FromFolder(path, "revosf2", {.jaudio1_effects = true}) : std::move(game);
}

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

// What a game's main.dol shows about a sound archive: the build of NW4R SND that plays it, the number of voices of the
// game's AX, and the game's profile.
struct GameCode
{
    std::optional<SndBuild> build;
    std::optional<int> voices;
    Profile profile = Profile::kNone;
};

// What a game's main.dol, `bytes`, shows. A main.dol without AX microcode shows neither the build nor the profile.
GameCode CodeOf(std::span<const uint8_t> bytes)
{
    GameCode code;
    code.build = ReadSndBuild(bytes);
    code.voices = ReadAxVoices(bytes);
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

// What the game's main.dol shows about the archive at `input`, as ArchiveCode finds it, without its messages. A
// main.dol that can't be read shows nothing.
GameCode QuietCode(const fs::path& input, const DolCode& given, const DolCode* game)
{
    if (given.code.build)
    {
        return given.code;
    }
    if (game)
    {
        return game->code;
    }

    const std::optional<fs::path> path = FindMainDol(input);
    const std::optional<std::vector<uint8_t>> bytes = path ? frame::ReadFile(*path) : std::nullopt;

    return bytes ? CodeOf(*bytes) : GameCode{};
}

// The input whose archive the frame lists: its path, and the main.dol of the game that holds it if the input was a
// game. The frame lists an input's sequences before it prepares the input. The list takes the profile from here.
struct ListedInput
{
    fs::path input;
    const DolCode* game = nullptr;
};

// `sequences` of `archive` with the variants of `variants` after their sounds (profiles.h). A variant takes the index
// of the archive's sound count plus its place in `variants`. It takes its sound's label and the variant's suffix as its
// name. A variant of a sound that the archive lacks isn't listed.
std::vector<frame::Sequence> WithVariants(const SoundArchive& archive, const std::vector<frame::Sequence>& sequences,
                                          std::span<const revosf2::Variant> variants)
{
    std::vector<frame::Sequence> listed;
    for (const frame::Sequence& sequence : sequences)
    {
        listed.push_back(sequence);
        for (std::size_t v = 0; v < variants.size(); v++)
        {
            const std::string& label = archive.Sounds()[sequence.index].name;
            if (variants[v].sound == label)
            {
                listed.push_back({archive.Sounds().size() + v, label + std::string(variants[v].suffix), {label}});
            }
        }
    }

    return listed;
}

// The index in `archive` of the sound labelled `label`. Throws if there's none.
uint32_t SoundIndex(const SoundArchive& archive, std::string_view label)
{
    const std::vector<ntdlib::rsar::SoundInfo>& sounds = archive.Sounds();
    for (uint32_t i = 0; i < sounds.size(); i++)
    {
        if (sounds[i].name == label)
        {
            return i;
        }
    }

    throw std::runtime_error("the archive holds no sound " + std::string(label));
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

// Whether `profile` silences tracks of any sound of `archive`.
bool SilencesTracks(Profile profile, const SoundArchive& archive)
{
    for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
    {
        PerformOptions options;
        revosf2::SetUpSound(profile, {}, sound.name, options);
        if (options.tracks != PerformOptions{}.tracks)
        {
            return true;
        }
    }

    return false;
}

// Whether `profile` sets a variable of any sound of `archive`.
bool SetsVariables(Profile profile, const SoundArchive& archive)
{
    for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
    {
        PerformOptions options;
        revosf2::SetUpSound(profile, {}, sound.name, options);
        if (!options.variables.empty())
        {
            return true;
        }
    }

    return false;
}

// Whether `profile` converts a sound of `archive` once more in a variant.
bool HasVariants(Profile profile, const SoundArchive& archive)
{
    for (const revosf2::Variant& variant : revosf2::VariantsOf(profile, false))
    {
        for (const ntdlib::rsar::SoundInfo& sound : archive.Sounds())
        {
            if (sound.name == variant.sound)
            {
                return true;
            }
        }
    }

    return false;
}

// Notes how `archive`, named `name`, plays its music by `profile`, in `arrangements`. The profiles of Wii Play, Wii
// Music, Wii Sports and Mario Kart Wii get a note only for an archive whose sounds they change. `chosen` is set when
// --weather or --museum gives the arrangements. They change nothing in an archive without City Folk's profile. The
// note says so.
void NoteProfile(const std::string& name, const SoundArchive& archive, Profile profile,
                 const Arrangements& arrangements, bool chosen)
{
    if (profile == Profile::kWiiPlay && SilencesTracks(profile, archive))
    {
        const char* states = HasVariants(profile, archive)
                                 ? " Its piece converts as the minigame starts it, and once more for each later set "
                                   "of tracks."
                                 : "";
        std::fprintf(stderr,
                     "note: %s plays its music as Wii Play's minigames play it.%s Tracks that the game mutes get CC11 "
                     "0.\n",
                     name.c_str(), states);
    }

    if (profile == Profile::kWiiMusic && SilencesTracks(profile, archive))
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

    if (profile == Profile::kWiiSports && SetsVariables(profile, archive))
    {
        std::fprintf(stderr,
                     "note: %s plays golf's wind, SE_LV_Wind, at a speed of 8 and in direction 0. Wii Sports sets "
                     "both by the hole. --var 0=N gives another speed (1 to 15), and --var 1=N another direction "
                     "(0 to 7).\n",
                     name.c_str());
    }

    if (profile == Profile::kMarioKart && SilencesTracks(profile, archive))
    {
        std::fprintf(stderr,
                     "note: %s plays the music of the menus and of the Wi-Fi menus as Mario Kart Wii starts it, and "
                     "once more in each of its other states. Tracks that the game silences get CC11 0. Six sound "
                     "effects play variable 0 at the value of their first play in the game.\n",
                     name.c_str());
    }

    if (profile == Profile::kCityFolk)
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
    else if (profile == Profile::kSmoothMoves)
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
    std::optional<jaudio::Game> opened = jaudio::Game::FromDiscFiles(game, "revosf2");
    if (!opened)
    {
        std::fprintf(stderr, "%s: the game holds no Wii sound archive (.brsar) and no JAudio start-up archive (.baa)\n",
                     frame::Utf8(path.filename()).c_str());
        return 1;
    }

    // A game of kEffectsGames opens a second time, with its effects.
    if (ListsEffects(*opened))
    {
        opened = jaudio::Game::FromDiscFiles(std::move(game), "revosf2", {.jaudio1_effects = true});
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
// converts as a JAudio game (ConvertJAudioGame). `several` is set when the command line has more than one input. Each
// archive becomes `listed` as it opens. Returns 1 on any failure, otherwise 0.
int ConvertGame(const std::string& input, const Options& options, const std::regex& only, bool several,
                UsedFolders& used_folders, const Converter& converter, const PrepareArchive& prepare,
                const JAudioInputs& jaudio_inputs, ListedInput& listed)
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
        archive_converter.open = [&game, &archive_path, &listed, &archive_input, &dol](
                                     const fs::path&, const std::string& archive_name) -> std::optional<SoundArchive>
        {
            listed = {archive_input, &dol};
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
        const jaudio::GameConverter& games, ListedInput& listed)
{
    // A JAudio game takes the same options. It leaves out --dol and --var (jaudio::ArchiveProgramOptions).
    const JAudioInputs jaudio_inputs{games, frame::ParseOptions<jaudio::PerformOptions>(args, games.option)};
    const frame::OtherInput<PerformOptions> other = [&converter, &prepare, &games, &jaudio_inputs, &listed](
                                                        const std::string& input, const Options& options,
                                                        const std::regex& only, bool several,
                                                        UsedFolders& used_folders) -> std::optional<int>
    {
        const fs::path path = frame::PathFromUtf8(input);
        if (revosf2::IsGameInput(path))
        {
            return ConvertGame(input, options, only, several, used_folders, converter, prepare, jaudio_inputs, listed);
        }
        if (jaudio::IsStartUpArchiveFile(path))
        {
            const jaudio::GameConverter archive_games = jaudio::MakeOpenGameConverter(
                games, [&path]() -> std::optional<jaudio::Game> { return OpenJAudioArchive(path); });
            return frame::ConvertInput(input, jaudio_inputs.options, only, several, used_folders, archive_games);
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
        "give the game, or the game's start-up archive (.baa or .aaf) with the Seqs\n"
        "folder and the Waves or Banks folder beside it. Pikmin has no start-up\n"
        "archive. Give the game.\n";
    program.extension = ".brsar";
    program.seed_option = "initial game random state (default 0x12345678)";
    program.extra_options =
        "  --dol FILE     the game's main.dol, for the build of the game's sound library\n"
        "                 (default: the game's main.dol when the input is a game, or\n"
        "                 sys/main.dol of the extracted disc that holds the archive)\n"
        "  --weather W    convert Animal Crossing: City Folk's hourly and town music in\n"
        "                 one weather: fine, rain or snow (default: all three)\n"
        "  --museum N     convert City Folk's museum music in one arrangement, from 1\n"
        "                 to 7 (default: 1 to 6)\n";

    // --dol names the game's main.dol. The build that it shows plays every archive. --weather and --museum choose the
    // arrangements of City Folk's music.
    DolCode dol;
    Arrangements arrangements;
    bool chose_arrangements = false;
    program.option = [&dol, &arrangements, &chose_arrangements](const std::string& option, frame::OptionReader& reader,
                                                                PerformOptions& perform)
    {
        if (option == "--dol")
        {
            dol.path = reader.Value();
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

            arrangements.weather = *weather;
            chose_arrangements = true;
            return true;
        }
        if (option == "--museum")
        {
            arrangements.museum = static_cast<int>(reader.Number(revosf2::kMuseumArrangements));
            if (arrangements.museum == 0)
            {
                throw std::runtime_error("--museum 0 is out of range");
            }

            chose_arrangements = true;
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
    const PrepareArchive prepare =
        [&dol, &arrangements, &chose_arrangements, &settings, &profile, &pieces](
            const fs::path& input, const SoundArchive& archive, bool quiet, const DolCode* game)
    {
        const GameCode code = ArchiveCode(input, archive, quiet, dol, game);
        if (!quiet)
        {
            NoteProfile(frame::Utf8(input.filename()), archive, code.profile, arrangements, chose_arrangements);
        }

        settings = frame::RevolutionSettings(code.build.value_or(frame::BuildOfRsarVersion(archive.Version())));
        settings.voices = code.voices.value_or(settings.voices);
        profile = code.profile;
        pieces = revosf2::FindPieces(archive);
    };

    // The frame's converter of a loose archive, from frame::MakeArchiveConverter. An input can be a game too. A loose
    // archive becomes `listed` as it opens. ConvertGame sets `listed` for a game's archives.
    Converter converter = frame::MakeArchiveConverter(program);
    converter.input_word = "input";
    ListedInput listed;
    const auto open_archive = converter.open;
    converter.open = [open_archive, &listed](const fs::path& input, const std::string& name)
    {
        listed = {input, nullptr};
        return open_archive(input, name);
    };

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

    // A piece of several sequences converts as one (pieces.h). A sound with variants converts once more for each
    // (profiles.h). The list takes the profile from the input's main.dol.
    converter.sequences = [&dol, &listed, &chose_arrangements](const SoundArchive& archive)
    {
        const Profile listed_profile = QuietCode(listed.input, dol, listed.game).profile;
        return WithVariants(archive, revosf2::ArchiveSequences(archive),
                            revosf2::VariantsOf(listed_profile, chose_arrangements));
    };
    converter.convert = [&settings, &profile, &arrangements, &chose_arrangements, &pieces](
                            SoundArchive& archive, std::size_t index, const PerformOptions& perform)
    {
        const std::size_t sounds = archive.Sounds().size();
        if (index >= sounds)
        {
            const std::span<const revosf2::Variant> variants = revosf2::VariantsOf(profile, chose_arrangements);
            if (index - sounds >= variants.size())
            {
                throw std::runtime_error("no variant " + std::to_string(index - sounds) + " in the game's profile");
            }

            const revosf2::Variant& variant = variants[index - sounds];
            PerformOptions options = perform;
            revosf2::SetUpSound(profile, arrangements, variant.sound, options);
            options.tracks = variant.tracks;
            return frame::ConvertSequence(archive, SoundIndex(archive, variant.sound), options, settings, "revosf2");
        }

        PerformOptions options = perform;
        revosf2::SetUpSound(profile, arrangements, archive.Sounds()[index].name, options);
        if (const std::optional<revosf2::Piece> piece = revosf2::PieceAt(pieces, index))
        {
            return frame::ConvertTogether(archive, piece->sounds, piece->name, options, settings, "revosf2");
        }

        return frame::ConvertSequence(archive, static_cast<uint32_t>(index), options, settings, "revosf2");
    };

    // The frame's converter of JAudio games, for a Wii game that plays its music with JAudio.
    const jaudio::GameConverter games = jaudio::MakeGameConverter(
        program.program, program.version_line, jaudio::ArchiveProgramOptions(converter.option), converter.usage);

    const auto run = [&converter, &prepare, &games, &listed](const std::vector<std::string>& args)
    {
        return Run(args, converter, prepare, games, listed);
    };
    return frame::RunMain(argc, argv, run);
}
