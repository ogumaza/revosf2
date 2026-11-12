// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h). ntdlib's game_setups.h holds the SHA-1s of the games' main.dol files and the
// tables of what the games set up for their sounds, with the addresses in the games' code. The profiles below set up
// the performance of a sound from those tables. Some set-ups use no table of ntdlib: the dry pieces of Smooth Moves,
// the steps of City Folk's gyroids, and the lost sends on bus B of the Mii Channel and the Wii Shop Channel.

#include "profiles.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/rvl/snd/game_setups.h"
#include "ntdlib/sequence_export/performance.h"
#include "xsflib/sha1.h"

namespace revosf2
{

namespace
{

namespace snd = ntdlib::rvl::snd;
using ntdlib::sequence_export::PerformOptions;

static_assert(snd::kCityFolkMuseumTracks.size() == kMuseumArrangements, "--museum names each of the arrangements");

// A game's main.dol, by its SHA-1, and the game's profile.
struct ProfileDol
{
    std::string_view sha1;
    Profile profile;
};

// The profile of each game's main.dol.
constexpr std::array<ProfileDol, 11> kProfileDols = {{
    {snd::kCityFolkSha1, Profile::kCityFolk},
    {snd::kWiiPlaySha1, Profile::kWiiPlay},
    {snd::kWiiPlayEuropeSha1, Profile::kWiiPlay},
    {snd::kWiiMusicSha1, Profile::kWiiMusic},
    {snd::kWiiMusicEuropeSha1, Profile::kWiiMusic},
    {snd::kMiiChannelSha1, Profile::kMiiChannel},
    {snd::kWiiShopChannelSha1, Profile::kWiiShopChannel},
    {snd::kWiiSportsSha1, Profile::kWiiSports},
    {snd::kMarioKartSha1, Profile::kMarioKart},
    {snd::kMarioKartEuropeSha1, Profile::kMarioKart},
    {snd::kSmoothMovesSha1, Profile::kSmoothMoves},
}};

// A send of -1 from a sound to a bus brings the sum of the sound send and each track send down to 0 or below. Every
// CC91 for bus A, or CC93 for bus B, then holds 0. The send stands in for a bus without an effect: AX mixes a bus into
// the output only once an effect's callback is set on it.
constexpr float kNoSend = -1.0f;

// The tracks that `starts` gives the sound named `sound` as the game starts it. Nothing if `starts` doesn't name it.
std::optional<uint16_t> StartTracksOf(std::span<const snd::StartTracks> starts, std::string_view sound)
{
    const auto start = std::ranges::find(starts, sound, &snd::StartTracks::sound);
    return start == starts.end() ? std::nullopt : std::optional<uint16_t>(start->tracks);
}

// Gives each variable that `values` names for the sound named `sound` its value, unless --var gives one.
void SetStartVariables(std::span<const snd::StartVariable> values, std::string_view sound, PerformOptions& perform)
{
    for (const snd::StartVariable& value : values)
    {
        if (value.sound == sound)
        {
            perform.variables.try_emplace(value.variable, value.value);
        }
    }
}

// The variants of a game's table `variants`. A variant's files add "_" and the words of its state to the sound's label,
// with "_" in place of each space. A variant holds no tempo. A state that plays at another tempo gets no conversion.
std::vector<Variant> VariantsIn(std::span<const snd::TrackVariant> variants)
{
    std::vector<Variant> found;
    for (const snd::TrackVariant& variant : variants)
    {
        if (variant.tempo_ratio != 1.0f)
        {
            continue;
        }

        std::string suffix = "_" + std::string(variant.state);
        std::ranges::replace(suffix, ' ', '_');
        found.push_back({variant.sound, std::move(suffix), variant.tracks});
    }

    return found;
}

// The gyroids' sounds (SEQ_HNW_*) play in step with the music. The game counts the music's steps in its global variable
// 2, a sequence's variable 18. The start of the music sets the count to 0 (0x801d4510, from 0x801cbc74). 0x801d4384
// reads the music's tick and adds 1 to the count each time the tick passes a step. After 15, the count goes back to 0
// (0x801d445c to 0x801d4494). A gyroid's sequence copies the variable as it starts. The sequence plays a step each time
// the variable changes. The profile plays one step: one frame after the start, the variable takes the count's next
// value. Without --var, the Wii's global variables start at 0.
constexpr uint8_t kStepVariable = 18;
constexpr int16_t kLastStep = 15;

// Smooth Moves' bus A keeps the reverb of the last piece of music that gave its variable 0 a value. 0x800fee28 reads
// the variable each frame. 0x80100e40 puts a row of the table at 0x804da11c on the bus for a value from 1 to 10, and
// clears the bus for 0. A sequence's variables start at -1 (0x800e58c8), and -1 leaves the bus as it is. The opening of
// each stage and the music between its microgames pick a row or clear the bus. The microgames' music picks nothing. In
// the game, each piece plays through its stage's reverb. The profile plays each piece as the stage that plays it first
// in the story does. For every microgame, that's the stage of its character: bits 0 to 9 of the flags at +0xc of its
// record in the table at 0x804dd970. Each character's boss ends that character's stage. The stage switch's table at
// 0x80547568 gives each stage's setup. The setups give the lists of microgames. The stages of Young Cricket, Penny,
// 9-Volt and Tiny Wario clear the bus. Dr. Crygor's stage clears it before its microgames' music
// (SEQ_M_TSUNAGI_CRYGOR_NEXT1_1). In a conversion, the pieces that these stages play first send nothing to the synth's
// reverb: CC91 0. Those of the other stages keep their sends. The list holds the pieces that send to bus A, by stage:
// Young Cricket's, Penny's, 9-Volt's and Tiny Wario's. Dr. Crygor's pieces are SEQ_M_PETIT_CRYGOR_GAME*.
constexpr std::array<std::string_view, 144> kSmoothMovesDryPieces = {
    "SEQ_M_BOSS_BLOCK1",
    "SEQ_M_BOSS_BLOCK1_OK",
    "SEQ_M_PETIT_BALLHOLE1",
    "SEQ_M_PETIT_BRAVE1",
    "SEQ_M_PETIT_BROOM1",
    "SEQ_M_PETIT_BROOM1_OK",
    "SEQ_M_PETIT_CASTANETS1_A_OK",
    "SEQ_M_PETIT_CASTANETS1_B_OK",
    "SEQ_M_PETIT_CASTANETS1_L1",
    "SEQ_M_PETIT_CASTANETS1_L1A",
    "SEQ_M_PETIT_CASTANETS1_L1B",
    "SEQ_M_PETIT_CASTANETS1_L2",
    "SEQ_M_PETIT_CASTANETS1_L2A",
    "SEQ_M_PETIT_CASTANETS1_L2A_US",
    "SEQ_M_PETIT_CASTANETS1_L3",
    "SEQ_M_PETIT_CASTANETS1_L3A",
    "SEQ_M_PETIT_CASTANETS1_L3B",
    "SEQ_M_PETIT_CASTANETS3_OK",
    "SEQ_M_PETIT_CURLING1",
    "SEQ_M_PETIT_DISHWASHER1",
    "SEQ_M_PETIT_EYEBEAM1",
    "SEQ_M_PETIT_HANDPLANE1",
    "SEQ_M_PETIT_SAW1",
    "SEQ_M_PETIT_STICK1",
    "SEQ_M_PETIT_STICK1_OK",
    "SEQ_M_PETIT_TRUNK1",
    "SEQ_M_PETIT_TWIDDLE1",
    "SEQ_M_PETIT_VOLLEY1",
    "SEQ_M_PETIT_YESNO1",
    "SEQ_M_BOSS_ESCAPE1",
    "SEQ_M_BOSS_ESCAPE1_A",
    "SEQ_M_BOSS_ESCAPE1_A_OK",
    "SEQ_M_BOSS_ESCAPE1_B",
    "SEQ_M_BOSS_ESCAPE1_B_OK",
    "SEQ_M_PETIT_AIRHOCKEY1",
    "SEQ_M_PETIT_CANOPENER1",
    "SEQ_M_PETIT_GASSTOVE1",
    "SEQ_M_PETIT_GLASS1",
    "SEQ_M_PETIT_GRIND1",
    "SEQ_M_PETIT_HAMMER1",
    "SEQ_M_PETIT_HAMMER1_OK",
    "SEQ_M_PETIT_INTERVIEW1",
    "SEQ_M_PETIT_SHREDDER1",
    "SEQ_M_PETIT_THERMOMETER1",
    "SEQ_M_PETIT_THROWINGKNIFE1",
    "SEQ_M_PETIT_TUNING2",
    "SEQ_M_PETIT_ZIP1",
    "SEQ_M_BOSS_STARFOX_BOSS1",
    "SEQ_M_BOSS_STARFOX_BOSS1_old",
    "SEQ_M_BOSS_STARFOX_CLEAR1",
    "SEQ_M_BOSS_STARFOX_CORNERIA1",
    "SEQ_M_BOSS_STARFOX_CORNERIA1_old",
    "SEQ_M_BOSS_STARFOX_L1",
    "SEQ_M_BOSS_STARFOX_L1_BOSS",
    "SEQ_M_BOSS_STARFOX_L2",
    "SEQ_M_BOSS_STARFOX_L2_BOSS",
    "SEQ_M_BOSS_STARFOX_L3",
    "SEQ_M_BOSS_STARFOX_L3_BOSS",
    "SEQ_M_BOSS_STARFOX_PLAYERDOWN1",
    "SEQ_M_BOSS_STARFOX_SECTERX1",
    "SEQ_M_BOSS_STARFOX_SECTERX1_old",
    "SEQ_M_BOSS_STARFOX_TITANIA1",
    "SEQ_M_BOSS_STARFOX_TITANIA1_old",
    "SEQ_M_PETIT_AGBMIW2_L1",
    "SEQ_M_PETIT_AGBMIW2_L2",
    "SEQ_M_PETIT_AGBMIW2_L3",
    "SEQ_M_PETIT_BRAINTRAINING1",
    "SEQ_M_PETIT_CLUCLULAND1",
    "SEQ_M_PETIT_CLUCLULAND1_OK",
    "SEQ_M_PETIT_CONDUCTOR1_A",
    "SEQ_M_PETIT_CONDUCTOR1_B",
    "SEQ_M_PETIT_CONDUCTOR1_C",
    "SEQ_M_PETIT_CONDUCTOR1_D",
    "SEQ_M_PETIT_CONDUCTOR1_E",
    "SEQ_M_PETIT_CONDUCTOR1_F",
    "SEQ_M_PETIT_CONDUCTOR1_F_1",
    "SEQ_M_PETIT_CONDUCTOR2_A",
    "SEQ_M_PETIT_CONDUCTOR2_B",
    "SEQ_M_PETIT_CONDUCTOR2_C",
    "SEQ_M_PETIT_CONDUCTOR3_A",
    "SEQ_M_PETIT_CONDUCTOR3_B",
    "SEQ_M_PETIT_CONDUCTOR3_C",
    "SEQ_M_PETIT_CONDUCTOR3_D",
    "SEQ_M_PETIT_CONTROLLER1",
    "SEQ_M_PETIT_C_METROID1",
    "SEQ_M_PETIT_FAMICOM1",
    "SEQ_M_PETIT_FIRE1",
    "SEQ_M_PETIT_FIRE_L1",
    "SEQ_M_PETIT_FIRE_L2",
    "SEQ_M_PETIT_FIRE_L3",
    "SEQ_M_PETIT_FISHING1",
    "SEQ_M_PETIT_METROID1",
    "SEQ_M_PETIT_METROID1_OK",
    "SEQ_M_PETIT_N64_ZELDA1_OK",
    "SEQ_M_PETIT_NINTENDOGS1",
    "SEQ_M_PETIT_OCARINA1",
    "SEQ_M_PETIT_OCTOPUS1",
    "SEQ_M_PETIT_PIKMIN1",
    "SEQ_M_PETIT_PIKMIN_OK",
    "SEQ_M_PETIT_PUNCHOUT1",
    "SEQ_M_PETIT_SIEVE1",
    "SEQ_M_PETIT_TAKT1",
    "SEQ_M_PETIT_TAKT_L1",
    "SEQ_M_PETIT_TAKT_L2",
    "SEQ_M_PETIT_TAKT_L3",
    "SEQ_M_PETIT_TENCOIN1",
    "SEQ_M_PETIT_TVGAME6",
    "SEQ_M_PETIT_TVGAME61",
    "SEQ_M_PETIT_WARIOLAND1",
    "SEQ_M_PETIT_WARIOLAND2",
    "SEQ_M_BOSS_DANCING1",
    "SEQ_M_BOSS_DANCING1_BUNKATU1",
    "SEQ_M_BOSS_DANCING1_BUNKATU2",
    "SEQ_M_BOSS_DANCING1_BUNKATU3",
    "SEQ_M_BOSS_DANCING1_BUNKATU4",
    "SEQ_M_BOSS_DANCING1_BUNKATU5",
    "SEQ_M_BOSS_DANCING1_BUNKATU6",
    "SEQ_M_BOSS_DANCING1_BUNKATU7",
    "SEQ_M_BOSS_DANCING1_BUNKATU8",
    "SEQ_M_BOSS_DANCING1_BUNKATU9",
    "SEQ_M_BOSS_DANCING1_OK",
    "SEQ_M_BOSS_DANCING1_SHORT",
    "SEQ_M_PETIT_ALWAYS1",
    "SEQ_M_PETIT_ALWAYS1_OK",
    "SEQ_M_PETIT_AUSCULTATION1",
    "SEQ_M_PETIT_AUSCULTATION1_OK",
    "SEQ_M_PETIT_BALL1",
    "SEQ_M_PETIT_BOW1",
    "SEQ_M_PETIT_BOWLING1",
    "SEQ_M_PETIT_HUNTING1",
    "SEQ_M_PETIT_KNOCK1",
    "SEQ_M_PETIT_MONEYHUNTER1",
    "SEQ_M_PETIT_MUSCLE1",
    "SEQ_M_PETIT_MUSCLE1_OK",
    "SEQ_M_PETIT_PAPERSUMO1",
    "SEQ_M_PETIT_RECYCLE1",
    "SEQ_M_PETIT_RING1",
    "SEQ_M_PETIT_RING1_OK",
    "SEQ_M_PETIT_SLAP1",
    "SEQ_M_PETIT_SLAP1_OK",
    "SEQ_M_PETIT_STENCH1",
    "SEQ_M_PETIT_SWORD1",
    "SEQ_M_PETIT_WAIST1",
    "SEQ_M_PETIT_WAIST1_OK",
};

void SetUpSmoothMovesSound(std::string_view sound, PerformOptions& perform)
{
    if (sound.starts_with("SEQ_M_PETIT_CRYGOR_GAME") ||
        std::ranges::find(kSmoothMovesDryPieces, sound) != kSmoothMovesDryPieces.end())
    {
        perform.fx_sends[0] = kNoSend;
    }
}

// City Folk's profile plays each hourly piece and each piece of town music with the tracks of the weather that
// --weather gives (snd::kCityFolkPieces), and the museum's music in the arrangement that --museum gives
// (snd::kCityFolkMuseumTracks). Without --weather and --museum, each piece converts once more in each arrangement that
// the game doesn't start it in (snd::kCityFolkVariants). The music sends to bus A are those that the game sets as it
// starts the music (snd::kCityFolkMusicSend and kCityFolkSongSend).
void SetUpCityFolkSound(const Arrangements& arrangements, std::string_view sound, PerformOptions& perform)
{
    if (sound.starts_with("SEQ_HNW_"))
    {
        const auto given = perform.variables.find(kStepVariable);
        const int step = given == perform.variables.end() ? 0 : given->second;
        const int16_t next = step < kLastStep ? static_cast<int16_t>(step + 1) : int16_t{0};
        perform.changes.push_back({.frame = 1, .index = kStepVariable, .value = next});
        return;
    }

    if (sound.starts_with(snd::kCityFolkSongPrefix))
    {
        perform.fx_sends[0] = snd::kCityFolkSongSend;
        return;
    }

    if (sound == snd::kCityFolkMuseum)
    {
        perform.tracks = snd::kCityFolkMuseumTracks[static_cast<std::size_t>(arrangements.museum - 1)];
        return;
    }

    const auto piece = std::ranges::find(snd::kCityFolkPieces, sound, &snd::WeatherTracks::sound);
    if (piece != snd::kCityFolkPieces.end())
    {
        perform.tracks = snd::CityFolkTracks(*piece, static_cast<std::size_t>(arrangements.weather));
        perform.fx_sends[0] = snd::kCityFolkMusicSend;
    }
}

// Six of Wii Play's minigames change the tracks of their music as a game goes on. A piece's conversion plays the tracks
// that sound as the music starts (snd::kWiiPlayStarts). Its variants play each later set of tracks
// (snd::kWiiPlayVariants). The variants _rally play the rally's arrangement of Table Tennis. During one player's rally,
// 0x80235540 also sets a tempo ratio of 0.96 to 1.14 by the rally's level. The conversions play a ratio of 1. Billiards
// halves the sound's volume in its states 2 and 3. The variants play at the sound's full volume. The addresses are
// those of the USA release. The European release has the same code, 0x110 bytes later: 0x80235650 there.
void SetUpWiiPlaySound(std::string_view sound, PerformOptions& perform)
{
    perform.tracks = StartTracksOf(snd::kWiiPlayStarts, sound).value_or(perform.tracks);
}

// Wii Music's main menu plays RP_MUSIC_MAINMENU_BACKTRACK of MusicStatic's rp_Music_sound.brsar with some of its
// tracks. Pitch Perfect plays its beats with some of theirs (snd::kWiiMusicStarts). Each sound's conversion plays the
// tracks that the game starts it with. Its variants play the others (snd::kWiiMusicVariants).
//
// Pitch Perfect's phrases and the jingles of its pictures send to bus A (snd::IsPitchPerfectSendSound and
// kPitchPerfectSend). In a jam session, the instrument engine sets sends on the instruments that a player plays
// (0x8036a8ec, 0x80373ce4 and 0x8041457c). The stage picks them. The archive doesn't give the stage. The conversions
// leave these sends out.
void SetUpWiiMusicSound(std::string_view sound, PerformOptions& perform)
{
    if (const std::optional<uint16_t> tracks = StartTracksOf(snd::kWiiMusicStarts, sound))
    {
        perform.tracks = *tracks;
    }
    else if (snd::IsPitchPerfectSendSound(sound))
    {
        perform.fx_sends[0] = snd::kPitchPerfectSend;
    }
}

// The Mii Channel's music, SEQ_TMP_BGM, converts with every track but 10 and 11 (snd::kMiiChannelStarts). Each variant
// plays the tracks of another state (snd::kMiiChannelVariants). The plaza's speed-up plays the tracks of boot at
// another tempo. It gets no conversion (VariantsIn). The channel also turns its music down to 0.4. The conversions play
// the music at its full volume.
//
// The channel's starters of sound effects send to bus A (snd::kMiiChannelEffectSend). The channel's reverb is there.
// AxManager::AppendEffect (0x800f1e40) has one caller, 0x80151074, and it puts the reverb on bus A. Nothing puts an
// effect on bus B. The sequences' sends to bus B are lost (kNoSend).
void SetUpMiiChannelSound(std::string_view sound, PerformOptions& perform)
{
    if (const std::optional<uint16_t> tracks = StartTracksOf(snd::kMiiChannelStarts, sound))
    {
        perform.tracks = *tracks;
    }
    else
    {
        perform.fx_sends[0] = snd::kMiiChannelEffectSend;
    }

    perform.fx_sends[1] = kNoSend;
}

// The Wii Shop Channel starts every sound without a StartInfo, and sets no send, no track and no variable. Its sound
// start-up (0x80023aa0) puts FxReverbHi on bus A (0x80023b80). Nothing puts an effect on bus B. The sequences' sends to
// bus B are lost.
void SetUpWiiShopChannelSound(PerformOptions& perform)
{
    perform.fx_sends[1] = kNoSend;
}

// Golf's wind plays at a speed of 8 and in direction 0 (snd::kWiiSportsWind). --var gives another speed or direction.
void SetUpWiiSportsSound(std::string_view sound, PerformOptions& perform)
{
    SetStartVariables(snd::kWiiSportsWind, sound, perform);
}

// Mario Kart Wii changes the tracks of two pieces of the music in its menus with the menus (snd::kMarioKartStarts).
// Each piece's conversion plays state 0. Its variants play the others (snd::kMarioKartVariants). In SEQ_O_SELECT's
// states 1 and 2, track 0 plays at 0.93 of its volume. A conversion can't hold that level, and plays it 0.63 dB louder.
// Six sound effects start with variable 0 at the value of their first play (snd::kMarioKartValues). --var gives another
// value.
void SetUpMarioKartSound(std::string_view sound, PerformOptions& perform)
{
    perform.tracks = StartTracksOf(snd::kMarioKartStarts, sound).value_or(perform.tracks);
    SetStartVariables(snd::kMarioKartValues, sound, perform);
}

} // namespace

std::optional<Weather> WeatherNamed(std::string_view name)
{
    constexpr std::array<std::pair<std::string_view, Weather>, 3> kNames = {
        {{"fine", Weather::kFine}, {"rain", Weather::kRain}, {"snow", Weather::kSnow}}};
    for (const auto& [weather_name, weather] : kNames)
    {
        if (name == weather_name)
        {
            return weather;
        }
    }

    return std::nullopt;
}

Profile ProfileOf(std::span<const uint8_t> dol)
{
    const std::string sha1 = xsflib::Sha1::Hex(xsflib::Sha1::Of(dol));
    const auto found = std::ranges::find(kProfileDols, std::string_view(sha1), &ProfileDol::sha1);

    return found == kProfileDols.end() ? Profile::kNone : found->profile;
}

std::vector<Variant> VariantsOf(Profile profile, bool chosen)
{
    if (profile == Profile::kCityFolk)
    {
        return chosen ? std::vector<Variant>() : VariantsIn(snd::kCityFolkVariants);
    }

    if (profile == Profile::kWiiPlay)
    {
        return VariantsIn(snd::kWiiPlayVariants);
    }

    if (profile == Profile::kWiiMusic)
    {
        return VariantsIn(snd::kWiiMusicVariants);
    }

    if (profile == Profile::kMiiChannel)
    {
        return VariantsIn(snd::kMiiChannelVariants);
    }

    return profile == Profile::kMarioKart ? VariantsIn(snd::kMarioKartVariants) : std::vector<Variant>();
}

void SetUpSound(Profile profile, const Arrangements& arrangements, std::string_view sound, PerformOptions& perform)
{
    if (profile == Profile::kCityFolk)
    {
        SetUpCityFolkSound(arrangements, sound, perform);
    }
    else if (profile == Profile::kWiiPlay)
    {
        SetUpWiiPlaySound(sound, perform);
    }
    else if (profile == Profile::kWiiMusic)
    {
        SetUpWiiMusicSound(sound, perform);
    }
    else if (profile == Profile::kMiiChannel)
    {
        SetUpMiiChannelSound(sound, perform);
    }
    else if (profile == Profile::kWiiShopChannel)
    {
        SetUpWiiShopChannelSound(perform);
    }
    else if (profile == Profile::kWiiSports)
    {
        SetUpWiiSportsSound(sound, perform);
    }
    else if (profile == Profile::kMarioKart)
    {
        SetUpMarioKartSound(sound, perform);
    }
    else if (profile == Profile::kSmoothMoves)
    {
        SetUpSmoothMovesSound(sound, perform);
    }
}

} // namespace revosf2
