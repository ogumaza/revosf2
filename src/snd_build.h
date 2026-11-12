// SPDX-License-Identifier: MIT

// The build of NW4R SND that plays a sequence (see the README, "Sequence playback").
//
// NW4R SND changed how it plays sequences after its builds of 2006, and again in March, June and November 2008 and in
// September 2009. An archive of version 1.4 or later comes from a build of 2008 or later. Older archives look the same
// whichever build plays them. A game's main.dol shows its build. NW4R SND's banner there gives the build's date. The
// builds of 2006 have no banner. ntdlib's sound_build.h reads the banner and AX's build. SndBuildWithAx there gives the
// build that a game plays by. ntdlib's RevolutionSettings gives the settings for a build.

#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_build.h"

namespace revosf2
{

// A build of NW4R SND, as sequence_export's Settings::wii_build holds it. Without a main.dol,
// ntdlib::sequence_export::BuildOfRsarVersion gives the build that the archive's version shows.
using SndBuild = ntdlib::SndBuild;

// The sound library that a main.dol links: the build of NW4R SND that plays its sequences (ntdlib's SndBuildWithAx),
// and the number of voices of AX.
//
// AX of October 2006 has 64 voices, and every later build 96. RevolutionSettings sets 64 voices for a build of October
// 2006. SndBuildWithAx gives that build to a game whose build has no banner. Such a game can have a later AX with 96
// voices.
struct SoundLibrary
{
    SndBuild build;
    int voices = 0;
};

// The sound library that a main.dol links, from one search for AX's microcode. Nothing if `dol` isn't a DOL holding
// AX's microcode.
std::optional<SoundLibrary> ReadSoundLibrary(std::span<const uint8_t> dol);

// The settings that play an archive of RSAR version `rsar_version`: those of `library`, the sound library of the
// game's main.dol, or else those of the build that the version gives (ntdlib's BuildOfRsarVersion).
ntdlib::sequence_export::Settings ArchiveSettings(const std::optional<SoundLibrary>& library, uint16_t rsar_version);

} // namespace revosf2
