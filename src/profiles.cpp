// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h).

#include "profiles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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
    return xsflib::Sha1::Hex(xsflib::Sha1::Of(dol)) == kCityFolkSha1 ? Profile::kCityFolk : Profile::kNone;
}

void SetUpSound(Profile profile, const Arrangements& arrangements, std::string_view sound, PerformOptions& perform)
{
    if (profile == Profile::kCityFolk)
    {
        SetUpCityFolkSound(arrangements, sound, perform);
    }
}

} // namespace revosf2
