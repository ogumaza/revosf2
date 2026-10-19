// SPDX-License-Identifier: MIT

// The profiles of games whose code sets up some of their sounds as it starts them, beyond what the sound archive gives.
// A profile is for one release: the main.dol whose SHA-1 it names. Animal Crossing: City Folk (USA) has one.

#pragma once

#include <cstdint>
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

// The profile of the game whose main.dol is `dol`.
Profile ProfileOf(std::span<const uint8_t> dol);

// Sets up the performance of the sound named `sound` as the game of `profile` starts it: the tracks that sound and the
// sound's sends. A sound that the game starts as the archive gives it keeps `perform` as it is.
void SetUpSound(Profile profile, std::string_view sound, ntdlib::sequence_export::PerformOptions& perform);

} // namespace revosf2
