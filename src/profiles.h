// SPDX-License-Identifier: MIT

// The profiles of games whose code sets up some of their sounds as it starts them, beyond what the sound archive gives.
// A profile is for one release: the main.dol whose SHA-1 it names. Animal Crossing: City Folk (USA) has one.

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
    kCityFolk, // Animal Crossing: City Folk (USA)
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

// Sets up the performance of the sound named `sound` as the game of `profile` starts it, in `arrangements`: the tracks
// that sound, the sound's sends, and the changes of variables that the game's code makes while the sound plays. A sound
// that the game starts as the archive gives it keeps `perform` as it is.
void SetUpSound(Profile profile, const Arrangements& arrangements, std::string_view sound,
                ntdlib::sequence_export::PerformOptions& perform);

} // namespace revosf2
