// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h).

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

#include "ntdlib/sequence_export/performance.h"
#include "xsflib/sha1.h"

namespace revosf2
{

namespace
{

using ntdlib::sequence_export::PerformOptions;

// Animal Crossing: City Folk (USA)'s main.dol.
constexpr std::string_view kCityFolkSha1 = "c87967b9ac0a943ba04ac16d875f5f79b4c46516";

// Wii Play's main.dol: the USA release (Rev 1) and the European release.
constexpr std::string_view kWiiPlaySha1 = "0da5e7e51135219f580ad011d1b635bc83569bb9";
constexpr std::string_view kWiiPlayEuropeSha1 = "323de3380c4ed9d26337d1219d46e527f2068d5e";

// Wii Music's main.dol: the USA release and the European release.
constexpr std::string_view kWiiMusicSha1 = "a9ab9a8a8f14aa58d9bf4e05d72d29115ec589ab";
constexpr std::string_view kWiiMusicEuropeSha1 = "2397197a29365b60d41f82b8f299ca5f2c4f9a2e";

// Mario Kart Wii's main.dol: the USA release and the European release. The code that the profile follows is in the
// game's module, files/rel/StaticR.rel. Each release holds one build of it.
constexpr std::string_view kMarioKartSha1 = "1b367209919fcc225c74a155dc46f29c5d64b9a2";
constexpr std::string_view kMarioKartEuropeSha1 = "ac7d72448630ade7655fc8bc5fd7a6543cb53a49";

// WarioWare: Smooth Moves (USA)'s main.dol.
constexpr std::string_view kSmoothMovesSha1 = "ab32545dc32724f1e4eeeecc05189af722b8addd";

// A piece of City Folk's music that the game plays with only some of its tracks: the tracks that sound in each weather,
// in the order of Weather.
struct PieceTracks
{
    std::string_view name;
    std::array<uint16_t, 3> tracks;
};

// City Folk's main.dol holds three masks of tracks for each hourly piece (BGM_OUTDOOR_00 to 23, at 0x8047e9f0) and for
// each piece of town music (BGM_TOWN_EARLY to BGM_TOWN_MIDNIGHT, at 0x8047ea90). As a piece starts, 0x801cc23c takes
// the mask of the music's state. 0x8007e000 gives state 1 for weather 0 to 2, state 2 for rain (3 and 4) and state 3
// for snow (5 and 6). The game then sets the tracks outside the mask to volume 0 (SeqSound::SetTrackVolume, from
// 0x801cc490).
constexpr std::array<PieceTracks, 28> kCityFolkPieces = {{
    {"BGM_OUTDOOR_00", {0x00ff, 0x01a0, 0x06a4}}, {"BGM_OUTDOOR_01", {0x07ff, 0x07f8, 0x1ff8}},
    {"BGM_OUTDOOR_02", {0x007f, 0x0187, 0x07a7}}, {"BGM_OUTDOOR_03", {0x03ff, 0x1cb8, 0x6cb8}},
    {"BGM_OUTDOOR_04", {0x009f, 0x00e4, 0x03e4}}, {"BGM_OUTDOOR_05", {0x002f, 0x00f4, 0x03a4}},
    {"BGM_OUTDOOR_06", {0x05ff, 0x1a48, 0x7a48}}, {"BGM_OUTDOOR_07", {0x09ff, 0x1e3c, 0x7e3c}},
    {"BGM_OUTDOOR_08", {0x447f, 0x05c3, 0xa5c3}}, {"BGM_OUTDOOR_09", {0x009f, 0x006d, 0x032d}},
    {"BGM_OUTDOOR_10", {0x04ff, 0x03d5, 0x1bd5}}, {"BGM_OUTDOOR_11", {0x13ff, 0x0edc, 0x6edc}},
    {"BGM_OUTDOOR_12", {0x00ff, 0x0338, 0x0f3c}}, {"BGM_OUTDOOR_13", {0x00ff, 0x4fb0, 0x7eb0}},
    {"BGM_OUTDOOR_14", {0x03ff, 0x0f28, 0x3f2a}}, {"BGM_OUTDOOR_15", {0x007f, 0x03ac, 0x0fac}},
    {"BGM_OUTDOOR_16", {0x07ff, 0x3d70, 0xf570}}, {"BGM_OUTDOOR_17", {0x02ff, 0x0d5c, 0x3d5c}},
    {"BGM_OUTDOOR_18", {0x077f, 0x03ac, 0x1b6c}}, {"BGM_OUTDOOR_19", {0x01ff, 0x0752, 0x1f52}},
    {"BGM_OUTDOOR_20", {0x007f, 0x00f8, 0x03f8}}, {"BGM_OUTDOOR_21", {0x1f7f, 0x6bc1, 0xebc1}},
    {"BGM_OUTDOOR_22", {0x19ff, 0x1870, 0x1e70}}, {"BGM_OUTDOOR_23", {0x003f, 0x183c, 0xfc3c}},
    {"BGM_TOWN_EARLY", {0x99ff, 0xc600, 0xe600}}, {"BGM_TOWN_DAY", {0x07ff, 0x3800, 0x7800}},
    {"BGM_TOWN_NIGHT", {0x01ff, 0x1c20, 0x1e20}}, {"BGM_TOWN_MIDNIGHT", {0x003f, 0x01c0, 0x03c0}},
}};

// The museum's music (BGM_MUSEUM) takes the mask of one of seven arrangements, the music's states 16 to 22, from
// 0x8047ea80. The game starts the music in the first (0x801cb9bc). The jump table at 0x80500528 then gives stages 31 to
// 36 and 38 states 17 to 21. Stages 30 and 37 keep state 16. No stage gets state 22.
constexpr std::array<uint16_t, kMuseumArrangements> kMuseumTracks = {0x00e7, 0x0063, 0x0063, 0x1863,
                                                                     0x007b, 0x0763, 0x0700};

// The start of the town music also sets its track 15 to volume 0 (0x801cbc34), in every weather. Only code that fades
// the track by the distance to a source (0x801cbec8) raises it again. Nothing in the game calls the functions that turn
// that code on (0x8007a778 and 0x80079294).
constexpr uint16_t kTownTrack15 = 0x8000;

// The variants of City Folk's music in the arrangements that the game doesn't start it in. Each hourly piece and each
// piece of town music converts once more in rain and in snow. The museum's music converts once more in each of its
// arrangements 2 to 6. Arrangements 2 and 3 play the same tracks. No stage plays arrangement 7.
constexpr std::size_t kCityFolkVariantCount = 2 * kCityFolkPieces.size() + 4;

constexpr std::array<Variant, kCityFolkVariantCount> CityFolkVariants()
{
    std::array<Variant, kCityFolkVariantCount> variants{};
    std::size_t v = 0;
    for (const PieceTracks& piece : kCityFolkPieces)
    {
        const uint16_t kept =
            piece.name.starts_with("BGM_TOWN_") ? static_cast<uint16_t>(~kTownTrack15) : uint16_t{0xffff};
        const auto in = [&piece, kept](Weather weather)
        {
            return static_cast<uint16_t>(piece.tracks[static_cast<std::size_t>(weather)] & kept);
        };
        variants[v++] = {piece.name, "_rain", in(Weather::kRain)};
        variants[v++] = {piece.name, "_snow", in(Weather::kSnow)};
    }

    variants[v++] = {"BGM_MUSEUM", "_arrangements_2_and_3", kMuseumTracks[1]};
    variants[v++] = {"BGM_MUSEUM", "_arrangement_4", kMuseumTracks[3]};
    variants[v++] = {"BGM_MUSEUM", "_arrangement_5", kMuseumTracks[4]};
    variants[v++] = {"BGM_MUSEUM", "_arrangement_6", kMuseumTracks[5]};

    return variants;
}

constexpr std::array<Variant, kCityFolkVariantCount> kCityFolkVariants = CityFolkVariants();
static_assert(kMuseumTracks[1] == kMuseumTracks[2], "arrangements 2 and 3 play the same tracks");

// The start of the music (0x801cbbbc) sets the sound's send to bus A, where the game's reverb is. The hourly and town
// music send 0.25. K.K. Slider's songs, sounds 0x1a29 to 0x1a77, send 0.28. The names of those songs start with BGM_KK.
constexpr float kMusicSend = 0.25f;
constexpr float kSongSend = 0.28f;

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

// A sound's send to bus A of -1 takes the sum of each track's send and the sound's to 0 or below. Every CC91 then holds
// 0. That stands in for the empty bus.
constexpr float kNoSend = -1.0f;

void SetUpSmoothMovesSound(std::string_view sound, PerformOptions& perform)
{
    if (sound.starts_with("SEQ_M_PETIT_CRYGOR_GAME") ||
        std::ranges::find(kSmoothMovesDryPieces, sound) != kSmoothMovesDryPieces.end())
    {
        perform.fx_sends[0] = kNoSend;
    }
}

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
    if (sound.starts_with("BGM_KK"))
    {
        perform.fx_sends[0] = kSongSend;
        return;
    }
    if (sound == "BGM_MUSEUM")
    {
        perform.tracks = kMuseumTracks[static_cast<std::size_t>(arrangements.museum - 1)];
        return;
    }

    for (const PieceTracks& piece : kCityFolkPieces)
    {
        if (piece.name != sound)
        {
            continue;
        }

        const uint16_t tracks = piece.tracks[static_cast<std::size_t>(arrangements.weather)];
        perform.tracks = sound.starts_with("BGM_TOWN_") ? static_cast<uint16_t>(tracks & ~kTownTrack15) : tracks;
        perform.fx_sends[0] = kMusicSend;

        return;
    }
}

// A piece of Wii Play's music that a minigame plays with only some of its tracks, and the tracks that sound.
struct PieceMask
{
    std::string_view name;
    uint16_t tracks;
};

// Six minigames change the tracks of their music as a game goes on, through SeqSound::SetTrackMute (0x8003fa7c) and
// SetTrackVolume (0x8003fa84). A piece's conversion plays the tracks that sound as the music starts (kWiiPlayPieces).
// Its variants play each later set of tracks (kWiiPlayVariants).
// - Pose Mii plays RP_BOM_BGM_PLAY (sound 0x22 of its sound archive) in its states 8 and 9 only (0x802dd7e0). Each
//   frame, 0x802dcef8 takes the mask of the state: 0x1 in states 8 and 9. Only track 0 plays.
// - Table Tennis starts RP_PNP_BGM_PLAY (sound 0x6b) for one player and RP_PNP_BGM_PLAY_VS (0x6c) for two (0x80235760,
//   by the mode at 0x8022d724). Both start without tracks 7 to 11 and 15 (0x802359b0). The rally's arrangement plays
//   tracks 0 to 3, 7 to 11, 14 and 15. 0x80235350 sets it: it mutes the tracks outside 0xcb8f (from 0x80235a14), lets
//   those of 0xcb8f | 0xc00 play and sets tracks 7, 8, 9, 11 and 15 to full volume. One player's game plays it at each
//   tenth count of a rally from 10 on (0x80235540). Two players' match plays it each time the ball goes into play (the
//   match's phase 4, 0x8022cdc4). Three bars later, the game fades tracks 7, 8, 9, 11 and 15 down to 0.02 (-34 dB) in
//   49 frames, mutes track 10 and lets tracks 4 to 6 play again: the start's tracks. Track 12 counts the bars in the
//   sequence's variable 0 (0x80235394). It plays no note, and the profile leaves it out. Once the match's higher score
//   reaches 11 (0x8022cdb4), the game no longer fades (0x802353e4), and the rally's arrangement stays. The conversion
//   plays the start's tracks. The variants _rally play the rally's arrangement. The game also sets the tempo of a rally
//   (the known problems' "Wii Play's tempos").
// - Laser Hockey starts RP_HKY_BGM_ALL_PLAY (sound 0x44) without tracks 0, 12, 13 and 14 (0x80220b04). Each frame,
//   0x8022074c lets tracks 0, 12 and 14 play once 30 s or less of the match are left. A match can also end at 8 goals
//   (0x8021a83c). In such a match, a player's seventh goal puts the music in state 3 (0x80216f1c). 0x8022074c then
//   lets tracks 0, 13 and 14 play. Nothing mutes those tracks again.
// - Billiards starts RP_BIL_BGM_PLAY (sound 0x4e) without tracks 3 and 4 (0x802c6b20). The game's state picks the
//   tracks (0x802c6988). On the beat after a change, 0x802c65a0 sets them. States 0, 1, 2 and 6 play the start's
//   tracks. State 3 adds track 3. It follows the crowd's booing (0x802c4918). State 4 plays every track. It follows a
//   cheer (0x802c4044). States 2 and 3 halve the sound's volume. The variants play at the sound's full volume. State 5
//   ends the game: a drum fill replaces the music (0x802c67f8).
// - Find Mii plays RP_WLY_BGM_PLAY_GUSU (sound 0x7a) as its time runs out (0x8028d8cc). On a beat, 0x8028d65c sets the
//   tracks of a mask from the table at 0x803247a0 to full volume and the others to 0. The seconds left pick the mask.
//   The music fades in at 20 s left (+0x18), or later if it starts with 25 s left or less. At 20 s, track 9 plays.
//   Track 0 joins at 18 s left, track 1 at 16 s, track 2 at 14 s and track 3 at 12 s. At 10 s, tracks 4 and 12 join.
//   At 6 s, only tracks 0, 3 and 4 play. The music then fades out. Track 11 never plays. The conversion plays track 9.
// - Tanks starts RP_TNK_BGM_LOOP2 (sound 0x3b) at 0x802682d0. 0x80268108 then plays the tracks in a cell of the table
//   at 0x80323298. The strongest kind of enemy tank still in play (1 to 9, 0x8025c914) picks the row. The number of
//   enemy tanks left, up to 4, picks the column. The first row plays tracks 0, 4 and 10 in each column. The conversion
//   plays them. Each other mask in the table gets a variant, in the table's order.
// The addresses are those of the USA release. The European release has the same code. The minigames' code there lies
// 0x110 bytes later (0x802dd008 and 0x80235460).
constexpr std::array<PieceMask, 7> kWiiPlayPieces = {{
    {"RP_BOM_BGM_PLAY", 0x0001},
    {"RP_PNP_BGM_PLAY", 0x007f},      // tracks 0 to 6
    {"RP_PNP_BGM_PLAY_VS", 0x4077},   // tracks 0, 1, 2, 4, 5, 6 and 14
    {"RP_HKY_BGM_ALL_PLAY", 0x003e},  // tracks 1 to 5
    {"RP_BIL_BGM_PLAY", 0x00a7},      // tracks 0, 1, 2, 5 and 7
    {"RP_WLY_BGM_PLAY_GUSU", 0x0200}, // track 9
    {"RP_TNK_BGM_LOOP2", 0x0411},     // tracks 0, 4 and 10
}};

constexpr std::string_view kTableTennis = "RP_PNP_BGM_PLAY";
constexpr std::string_view kTableTennisVs = "RP_PNP_BGM_PLAY_VS";
constexpr std::string_view kHockey = "RP_HKY_BGM_ALL_PLAY";
constexpr std::string_view kBilliards = "RP_BIL_BGM_PLAY";
constexpr std::string_view kFindMii = "RP_WLY_BGM_PLAY_GUSU";
constexpr std::string_view kTanks = "RP_TNK_BGM_LOOP2";
constexpr std::array<Variant, 34> kWiiPlayVariants = {{
    {kTableTennis, "_rally", 0x0f8f},   // tracks 0 to 3 and 7 to 11
    {kTableTennisVs, "_rally", 0xcf87}, // tracks 0, 1, 2, 7 to 11, 14 and 15
    {kHockey, "_last_30_s", 0x503f},
    {kHockey, "_match_point", 0x603f},
    {kHockey, "_last_30_s_at_match_point", 0x703f},
    {kBilliards, "_state_3", 0x00af},
    {kBilliards, "_state_4", 0x00bf},
    {kFindMii, "_17_or_18_s_left", 0x0201},
    {kFindMii, "_15_or_16_s_left", 0x0203},
    {kFindMii, "_13_or_14_s_left", 0x0207},
    {kFindMii, "_11_or_12_s_left", 0x020f},
    {kFindMii, "_7_to_10_s_left", 0x121f},
    {kFindMii, "_last_6_s", 0x0019},
    {kTanks, "_arrangement_2", 0x0491},
    {kTanks, "_arrangement_3", 0x0493},
    {kTanks, "_arrangement_4", 0x04c3},
    {kTanks, "_arrangement_5", 0x04d3},
    {kTanks, "_arrangement_6", 0x04d9},
    {kTanks, "_arrangement_7", 0x0419},
    {kTanks, "_arrangement_8", 0x0459},
    {kTanks, "_arrangement_9", 0x04e3},
    {kTanks, "_arrangement_10", 0x04f3},
    {kTanks, "_arrangement_11", 0x04f9},
    {kTanks, "_arrangement_12", 0x0543},
    {kTanks, "_arrangement_13", 0x0573},
    {kTanks, "_arrangement_14", 0x05f9},
    {kTanks, "_arrangement_15", 0x0547},
    {kTanks, "_arrangement_16", 0x0567},
    {kTanks, "_arrangement_17", 0x05f7},
    {kTanks, "_arrangement_18", 0x05fd},
    {kTanks, "_arrangement_19", 0x0713},
    {kTanks, "_arrangement_20", 0x07b9},
    {kTanks, "_arrangement_21", 0x07f9},
    {kTanks, "_arrangement_22", 0x07ed},
}};

void SetUpWiiPlaySound(std::string_view sound, PerformOptions& perform)
{
    for (const PieceMask& piece : kWiiPlayPieces)
    {
        if (piece.name == sound)
        {
            perform.tracks = piece.tracks;
            return;
        }
    }
}

// Wii Music's main menu plays RP_MUSIC_MAINMENU_BACKTRACK (sound 0x42c of MusicStatic's rp_Music_sound.brsar,
// 0x803cced8) with some of its 15 tracks. The menu's state (+0x104 of the menu's object, from the menu's choice at
// 0x80299b8c) picks them. As a state starts, 0x8029da6c sets levels for groups of tracks. Each frame, 0x8029de40 fades
// the levels toward the state's targets over 60 frames. It then sets the tracks' volumes: tracks 1 to 5 to the larger
// of two levels, track 6 to the largest of three, tracks 8 to 11 and 12 to 14 to one level each. Tracks 0 and 7 always
// play. Each state's targets give its tracks:
// - States 0, 2 and 4 play tracks 0 to 7. The game starts the menu in state 0. Its carousel plays tracks 1 to 6 at
//   0.74 or 0.51, by the stop that faces the front. State 2 plays them at 0.9.
// - State 1 plays tracks 0, 7 and 8 to 11.
// - State 3 plays tracks 0, 6, 7 and 12 to 14.
// - States 5, 6 and 7 play tracks 0 and 7.
// The game never plays tracks 8 to 14 together. The sound's conversion plays state 0. Its variants play the others.
//
// Pitch Perfect plays RP_KKM_BEATS_LOOP (sound 0x352) through the music manager (0x8021d60c). In its modes 12 and 13,
// it plays RP_KKM_BEATS_LOOP_MAKE_SCORE (0x353) instead. As the loop starts, the game silences the tracks outside a
// mask through the music manager's track fader (0x801bc904). Tracks 0, 1 and 3 always play. Track 4 joins at levels 2
// and up, track 5 at levels 4 and up, and track 6 at levels 7 to 9. The level is +0x8 of the round's record. Each
// frame, 0x8021e058 lets track 2 play once the round's time (+0xc of the record) reads 10 s or less. Each sound's
// conversion plays tracks 0, 1 and 3. Its variants play the other masks. RP_KKM_BEATS_LOOP_MAKE_SCORE has no tracks 5
// and 6.
constexpr std::string_view kMenuTrack = "RP_MUSIC_MAINMENU_BACKTRACK";
constexpr uint16_t kMenuTracks = 0x00ff;
constexpr std::string_view kBeats = "RP_KKM_BEATS_LOOP";
constexpr std::string_view kBeatsScore = "RP_KKM_BEATS_LOOP_MAKE_SCORE";
constexpr uint16_t kBeatsTracks = 0x000b;
constexpr std::array<Variant, 13> kWiiMusicVariants = {{
    {kMenuTrack, "_state_1", 0x0f81},
    {kMenuTrack, "_state_3", 0x70c1},
    {kMenuTrack, "_states_5_to_7", 0x0081},
    {kBeats, "_levels_2_and_3", 0x001b},
    {kBeats, "_levels_4_to_6", 0x003b},
    {kBeats, "_levels_7_to_9", 0x007b},
    {kBeats, "_with_track_2", 0x000f},
    {kBeats, "_levels_2_and_3_with_track_2", 0x001f},
    {kBeats, "_levels_4_to_6_with_track_2", 0x003f},
    {kBeats, "_levels_7_to_9_with_track_2", 0x007f},
    {kBeatsScore, "_levels_2_to_9", 0x001b},
    {kBeatsScore, "_with_track_2", 0x000f},
    {kBeatsScore, "_levels_2_to_9_with_track_2", 0x001f},
}};

// Pitch Perfect starts its phrases and the jingles of its pictures through 0x8021d138: RP_KKM_SE_PHRASE_1_1 to 1_6 and
// the 13 sounds named RP_KKM_SE_IMG_*. 0x8021d04c picks them from its jump table. 0x8021d138 then sets the sound's send
// to bus A to the scene's send for that bus (0x803cded0). As Pitch Perfect starts, it sets up the buses from row 14 of
// the scenes' table at 0x8059f978 (0x8021e30c). That row puts the reverb on bus A, with a send of 0.2. In a jam
// session, the instrument engine sets sends on the instruments that a player plays (0x8036a8ec, 0x80373ce4 and
// 0x8041457c). The stage picks them. A conversion doesn't know the stage and leaves them out.
constexpr float kPitchPerfectSend = 0.2f;

bool IsPitchPerfectJingle(std::string_view sound)
{
    return sound.starts_with("RP_KKM_SE_PHRASE_1_") || sound.starts_with("RP_KKM_SE_IMG_");
}

void SetUpWiiMusicSound(std::string_view sound, PerformOptions& perform)
{
    if (sound == kMenuTrack)
    {
        perform.tracks = kMenuTracks;
    }
    else if (sound == kBeats || sound == kBeatsScore)
    {
        perform.tracks = kBeatsTracks;
    }
    else if (IsPitchPerfectJingle(sound))
    {
        perform.fx_sends[0] = kPitchPerfectSend;
    }
}

// A sound's send of -1 on a bus takes each track's send on that bus to 0. A channel whose code puts no effect on bus B
// loses the sends that its sequences make there: AX mixes a bus into the output only once an effect's callback is set
// on it.
constexpr float kLostSend = -1.0f;

// The Mii Channel's main program (World, version 6).
constexpr std::string_view kMiiChannelSha1 = "5e0325b32438065bbc33fb82b755a250d93a9f9b";

// The Mii Channel's music, SEQ_TMP_BGM, starts without a send (0x80018a2c). As it starts, the channel silences its
// tracks 9, 10 and 11 (SeqSoundHandle::SetTrackSilence, 0x800fdf0c). The bytes at +0x11b8 to +0x11ba of the channel's
// sound object say which of them play. The scene function (0x80018018, by the jump table at 0x80233a20) sets them
// again. At boot it lets track 9 play. The conversion plays every track but 10 and 11. Each variant plays the tracks of
// another state:
// - _scene_4: track 10 alone of the three. Scene 4 sets it (0x8002c2d8).
// - _menu_out: tracks 9 and 11. The channel lets track 11 play as it plays SEQ_DOOL_MENU_OUT, and silences it again
//   with SEQ_DOOL_INTO_MENU. Neither touches tracks 9 and 10.
// - _scenes_1_and_3: none of the three (0x80011b30 and 0x8001755c).
// The plaza's speed-up plays the tracks of boot at a tempo ratio of 1.0878 (0x8001862c). The channel also turns its
// music down to 0.4 (0x8001868c). A variant holds neither a tempo nor a volume. Those states get no conversion.
//
// Each of the channel's four starters of sound effects (0x80018bb4, 0x80018cf8, 0x80018ddc and 0x80019474) sends 0.2 of
// its sound to bus A (BasicSound::SetFxSend, 0x800f73a0). The channel's reverb is there. AxManager::AppendEffect
// (0x800f1e40) has one caller, 0x80151074, and it puts the reverb on bus A. Nothing puts an effect on bus B.
constexpr std::string_view kMiiMusic = "SEQ_TMP_BGM";
constexpr uint16_t kMiiMusicTracks = 0xf3ff;
constexpr float kMiiEffectSend = 0.2f;
constexpr std::array<Variant, 3> kMiiChannelVariants = {{
    {kMiiMusic, "_scene_4", 0xf5ff},
    {kMiiMusic, "_menu_out", 0xfbff},
    {kMiiMusic, "_scenes_1_and_3", 0xf1ff},
}};

void SetUpMiiChannelSound(std::string_view sound, PerformOptions& perform)
{
    if (sound == kMiiMusic)
    {
        perform.tracks = kMiiMusicTracks;
    }
    else
    {
        perform.fx_sends[0] = kMiiEffectSend;
    }

    perform.fx_sends[1] = kLostSend;
}

// The Wii Shop Channel's main program (World, version 20).
constexpr std::string_view kWiiShopChannelSha1 = "aecf1c655b67c6412998cbc71bf58eba2e4edf03";

// The Wii Shop Channel starts every sound without a StartInfo, and sets no send, no track and no variable. Its sound
// start-up (0x80023aa0) puts FxReverbHi on bus A (0x80023b80). Nothing puts an effect on bus B. The sequences' sends to
// bus B are lost.
void SetUpWiiShopChannelSound(PerformOptions& perform)
{
    perform.fx_sends[1] = kLostSend;
}

// Wii Sports' main.dol (USA, Rev 1).
constexpr std::string_view kWiiSportsSha1 = "8bb422971b88b5551a37de98db69557df7b46637";

// Golf's wind, SE_LV_Wind, plays nothing while its variable 0 is 0. In every frame, golf holds the sound while the
// hole's wind is 1 or more (0x802b8c38). It writes the wind's speed to variable 0, and the wind's direction from the
// camera, 0 to 7, to variable 1 (0x802b7df8, from 0x802b8cb0). The courses draw each hole's speed from 0 to 15, or from
// part of that range (0x8029ced4 to 0x8029cf20, through 0x8029dcf4). The conversion plays a speed of 8, the middle of
// the speeds that sound, and direction 0. A higher speed plays the gusts louder and higher, with shorter waits between
// them. The direction sets how the gusts pan. --var gives another speed or direction.
constexpr std::string_view kWind = "SE_LV_Wind";
constexpr uint8_t kWindSpeedVariable = 0;
constexpr uint8_t kWindDirectionVariable = 1;
constexpr int16_t kWindSpeed = 8;
constexpr int16_t kWindDirection = 0;

void SetUpWiiSportsSound(std::string_view sound, PerformOptions& perform)
{
    if (sound == kWind)
    {
        perform.variables.try_emplace(kWindSpeedVariable, kWindSpeed);
        perform.variables.try_emplace(kWindDirectionVariable, kWindDirection);
    }
}

// Mario Kart Wii changes the tracks of two pieces of its menus' music with the menus: SEQ_O_SELECT (sound 0x55 of
// revo_kart.brsar), the music of the menus, and SEQ_O_EARTH (sound 1), the music of the Wi-Fi menus. The addresses
// below are offsets in the sections of the USA release's StaticR.rel. The European release has the same code and
// tables at other offsets.
// - Each section of the menus plays the music of a group of the archive (.text+0xf5010). SEQ_O_SELECT is in group 7
//   (GRP_MENU), the group of sections 0x48 to 0x54. SEQ_O_EARTH is in group 3, that of sections 0x55 to 0x67 and 0x7f
//   to 0x8b.
// - The game gives each page of the menus a state from 0 to 6 (.text+0xf52cc). As a page opens, the menus' sound scene
//   (.text+0x20089c) keeps the page and fades the music's tracks to the state's volumes in 30 frames (.text+0x1e9b90).
//   States 4 and 6 change nothing. State 5 turns the music down to 0.55. In the Wi-Fi menus, pages 0x6e, 0x6f, 0x78
//   and 0x79 take state 3.
// - As the music starts, .text+0x1e814c sets the volumes of the tracks (SeqSound::SetTrackVolume, through
//   .text+0x1e92bc). SEQ_O_SELECT takes those of the kept page's state: state 1 or 2, or else state 0. SEQ_O_EARTH
//   takes those of the menus' section: state 2 in sections 0x56 and 0x57, state 3 in sections 0x58, 0x59 and 0x5e to
//   0x67, and state 0 in the others.
// - The volumes of each state are in the table at .rodata+0x11508. SEQ_O_SELECT's state 0 plays track 0 alone, state 1
//   tracks 0, 1 and 3, and state 2 tracks 0, 1 and 2. In states 1 and 2, track 0 plays at 0.93 of its volume. A
//   conversion can't hold that level, and plays it 0.63 dB louder. SEQ_O_EARTH's state 0 plays tracks 1, 2, 4 and 6,
//   state 1 tracks 0, 1, 2, 4, 6, 8, 9 and 11, state 2 tracks 1, 2, 4, 5, 6, 8, 9, 11 and 13, and state 3 every track.
// The main menu (section 0x48, page 0x69) is in state 0. The Wi-Fi menus start in section 0x55. Each piece's
// conversion plays state 0. Its variants play the others, in the order in which the menus first reach them.
constexpr std::string_view kSelect = "SEQ_O_SELECT";
constexpr std::string_view kEarth = "SEQ_O_EARTH";
constexpr uint16_t kSelectTracks = 0x0001;
constexpr uint16_t kEarthTracks = 0x0056;
constexpr std::array<Variant, 5> kMarioKartVariants = {{
    {kSelect, "_state_1", 0x000b},
    {kSelect, "_state_2", 0x0007},
    {kEarth, "_state_1", 0x0b57},
    {kEarth, "_state_2", 0x2b76},
    {kEarth, "_state_3", 0x3fff},
}};

// Seven of Mario Kart Wii's sound effects read their variable 0. The game writes it with a value for each play, as it
// starts the effect, through its wrapper of SeqSoundHandle::WriteVariable (.text+0x2041fc). A conversion plays the
// value of the effect's first play. Where play decides it, that is the first play from the start of a Grand Prix's
// first race, with controller 1:
// - SE_CN_RBW_RING gets 0, 2, 4, 6 or 8 by the ring (.text+0x206c4c to 0x206cdc). The first ring gives 0.
// - SE_CN_RBW_STAR_RING gets 2 times the ring's count (.text+0x1f60c0), or a parameter times the count
//   (.text+0x1faea0). The count starts at 0 (.text+0x1af120 and 0x1af8d4).
// - SE_RC_RANK_UP and SE_RC_RANK_DOWN get 12 minus the new rank (.text+0x202c10). The rank counts from 1: the HUD
//   shows frame rank - 1 (.text+0x2de2e0). A lower rank than the last plays RANK_UP, any other change RANK_DOWN
//   (.text+0x2de488). From 12th on the grid, the first RANK_UP reaches 11th and gives 1. The first RANK_DOWN can come
//   only after it. At the earliest, it returns to 12th and gives 0.
// - SE_UI_BTN_OK and SE_UI_CTRL_ON get the controller's channel, 0 to 3 (.text+0x2018d0 to 0x201940). Controller 1
//   gives 0.
// SE_CC_MSR_GATE_PASS keeps -1. It gets the number of gates on the course minus the gate's number (.text+0x36e2e4).
// The course's file gives both. --var gives another value.
struct MarioKartValue
{
    std::string_view label;
    int16_t value;
};

constexpr uint8_t kMarioKartVariable = 0;
constexpr std::array<MarioKartValue, 6> kMarioKartValues = {{
    {"SE_CN_RBW_RING", 0},
    {"SE_CN_RBW_STAR_RING", 0},
    {"SE_RC_RANK_UP", 1},
    {"SE_RC_RANK_DOWN", 0},
    {"SE_UI_BTN_OK", 0},
    {"SE_UI_CTRL_ON", 0},
}};

void SetUpMarioKartSound(std::string_view sound, PerformOptions& perform)
{
    if (sound == kSelect)
    {
        perform.tracks = kSelectTracks;
    }
    else if (sound == kEarth)
    {
        perform.tracks = kEarthTracks;
    }

    for (const MarioKartValue& value : kMarioKartValues)
    {
        if (sound == value.label)
        {
            perform.variables.try_emplace(kMarioKartVariable, value.value);
        }
    }
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
    if (sha1 == kCityFolkSha1)
    {
        return Profile::kCityFolk;
    }

    if (sha1 == kWiiMusicSha1 || sha1 == kWiiMusicEuropeSha1)
    {
        return Profile::kWiiMusic;
    }

    if (sha1 == kMiiChannelSha1)
    {
        return Profile::kMiiChannel;
    }

    if (sha1 == kWiiShopChannelSha1)
    {
        return Profile::kWiiShopChannel;
    }

    if (sha1 == kWiiSportsSha1)
    {
        return Profile::kWiiSports;
    }

    if (sha1 == kMarioKartSha1 || sha1 == kMarioKartEuropeSha1)
    {
        return Profile::kMarioKart;
    }

    if (sha1 == kSmoothMovesSha1)
    {
        return Profile::kSmoothMoves;
    }

    return sha1 == kWiiPlaySha1 || sha1 == kWiiPlayEuropeSha1 ? Profile::kWiiPlay : Profile::kNone;
}

std::span<const Variant> VariantsOf(Profile profile, bool chosen)
{
    if (profile == Profile::kCityFolk)
    {
        return chosen ? std::span<const Variant>() : std::span<const Variant>(kCityFolkVariants);
    }

    if (profile == Profile::kWiiPlay)
    {
        return kWiiPlayVariants;
    }

    if (profile == Profile::kWiiMusic)
    {
        return kWiiMusicVariants;
    }

    if (profile == Profile::kMiiChannel)
    {
        return kMiiChannelVariants;
    }

    return profile == Profile::kMarioKart ? std::span<const Variant>(kMarioKartVariants) : std::span<const Variant>();
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
