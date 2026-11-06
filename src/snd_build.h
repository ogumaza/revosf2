// SPDX-License-Identifier: MIT

// The build of NW4R SND that plays a sequence (see the README, "Sequence playback").
//
// NW4R SND changed how it plays sequences after its builds of 2006, and again in March, June and November 2008 and in
// September 2009. An archive of version 1.4 or later comes from a build of 2008 or later. Older archives look the same
// whichever build plays them. A game's main.dol shows its build. NW4R SND's banner there gives the build's date. The
// builds of 2006 have no banner. AX of October 2006 transfers 0x120-byte parameter blocks, and every later AX transfers
// 0x140 bytes. Some builds without a banner come with a later AX. ntdlib's sound_build.h reads the banner and AX's
// build. ntdlib's RevolutionSettings gives the settings for a build.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

#include "ntdlib/sound_build.h"

namespace revosf2
{

// A build of NW4R SND, as sequence_export's Settings::wii_build holds it. Without a main.dol,
// ntdlib::sequence_export::BuildOfRsarVersion gives the build that the archive's version shows.
using SndBuild = ntdlib::SndBuild;

// The build that a main.dol links, or nothing if `dol` isn't a DOL holding AX's microcode. A build without a banner
// plays as the build of October 2006, whatever AX comes with it. Its sequence clock counts in 416ths of a tick, as that
// build's does.
std::optional<SndBuild> ReadSndBuild(std::span<const uint8_t> dol);

// The number of voices of the AX that a main.dol links: 64 for AX of October 2006 and 96 for every later build. Nothing
// if `dol` isn't a DOL holding AX's microcode. RevolutionSettings gives a build of October 2006 64 voices. A build
// without a banner can come with a later AX and its 96 voices.
std::optional<int> ReadAxVoices(std::span<const uint8_t> dol);

// The main.dol of the extracted disc that holds `archive`: sys/main.dol in a folder whose files folder holds the
// archive. Dolphin and wit extract a disc that way. Nothing if there's none.
std::optional<std::filesystem::path> FindMainDol(const std::filesystem::path& archive);

} // namespace revosf2
