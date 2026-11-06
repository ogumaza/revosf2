// SPDX-License-Identifier: MIT

// The profiles of games whose code sets up some of their sounds as it starts them, beyond what the sound archive gives.
// A profile is for the releases whose main.dol it names by SHA-1. Animal Crossing: City Folk (USA) has one. Wii Play
// has one for its USA release (Rev 1) and its European release. Wii Music and Mario Kart Wii each have one for their
// USA and European releases. The Mii Channel (World, version 6) and the Wii Shop Channel (World, version 20) have one
// each. A channel is a title, and the title's main program serves as its main.dol. Wii Sports (USA, Rev 1) and
// WarioWare: Smooth Moves (USA) have one each.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "ntdlib/sequence_export/performance.h"

namespace revosf2
{

// The games that have a profile.
enum class Profile : uint8_t
{
    kNone,
    kCityFolk,       // Animal Crossing: City Folk (USA)
    kWiiPlay,        // Wii Play (USA Rev 1, and Europe)
    kWiiMusic,       // Wii Music (USA and Europe)
    kMiiChannel,     // the Mii Channel (World, version 6)
    kWiiShopChannel, // the Wii Shop Channel (World, version 20)
    kWiiSports,      // Wii Sports (USA Rev 1)
    kMarioKart,      // Mario Kart Wii (USA and Europe)
    kSmoothMoves,    // WarioWare: Smooth Moves (USA)
};

// The weather that City Folk plays its hourly and town music in. The game picks the tracks of each piece by it.
enum class Weather : uint8_t
{
    kFine,
    kRain,
    kSnow,
};

// The arrangements of City Folk's museum music. The game picks one by room.
constexpr int kMuseumArrangements = 7;

// The arrangements that the profiles play, from --weather and --museum. The defaults are those that the game starts its
// music in.
struct Arrangements
{
    Weather weather = Weather::kFine;
    int museum = 1; // from 1 to kMuseumArrangements
};

// The weather named `name`: "fine", "rain" or "snow". Nothing for another name.
std::optional<Weather> WeatherNamed(std::string_view name);

// The profile of the game whose main.dol is `dol`.
Profile ProfileOf(std::span<const uint8_t> dol);

// A sound that a game plays in more than one arrangement converts once in each. The sound's conversion plays the
// arrangement that SetUpSound gives. A variant names another: the sound's label, the suffix that the variant's files
// add to it, and the tracks that sound.
struct Variant
{
    std::string_view sound;
    std::string_view suffix;
    uint16_t tracks;
};

// The variants that `profile` converts, in the order that the conversions list them. City Folk's profile converts its
// variants only in the arrangements that the game starts in. `chosen` is set when --weather or --museum gives others.
std::span<const Variant> VariantsOf(Profile profile, bool chosen);

// Sets up the performance of the sound named `sound` as the game of `profile` starts it, in `arrangements`: the tracks
// that sound, the sound's sends, and the changes of variables that the game's code makes while the sound plays. A sound
// that the game starts as the archive gives it keeps `perform` as it is.
void SetUpSound(Profile profile, const Arrangements& arrangements, std::string_view sound,
                ntdlib::sequence_export::PerformOptions& perform);

} // namespace revosf2
