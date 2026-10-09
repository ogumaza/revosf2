// SPDX-License-Identifier: MIT

// The build of NW4R SND that plays a sequence (see the README, "Sequence playback").
//
// NW4R SND changed how it plays sequences after its builds of 2006, and again in March and June 2008. An archive of
// version 1.4 or later comes from a build of 2008. Older archives look the same whichever build plays them. A game's
// main.dol shows its build. NW4R SND's banner there gives the build's date. AX's microcode marks the builds of 2006:
// their AX transfers 0x120-byte parameter blocks, and every later one transfers 0x140 bytes. The WSF project identifies
// the builds the same way.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace revosf2
{

// A build of NW4R SND.
struct SndBuild
{
    // Builds from March 2008 (Mario Kart Wii) add time prefixes, the envelope's hold, monophony, note-off types, key
    // groups, sweep_pitch in 64ths and a tempo limit.
    bool Since2008() const
    {
        return date >= 20080308;
    }

    // Builds from June 2008 (Wii Music) add the velocity range, the biquad filter and the envelope reset.
    bool SinceJune2008() const
    {
        return date >= 20080620;
    }

    uint32_t date = 0;    // the date in NW4R SND's banner, as YYYYMMDD, or 0 without one
    bool ax_2006 = false; // a build of 2006, with AX of October 2006
};

// The build that an archive of version `version` comes from, without a main.dol: a build from June 2008 for version 1.4
// or later, and otherwise a build of 2007.
SndBuild BuildOfVersion(uint16_t version);

// The build that a main.dol links, or nothing if `dol` isn't a DOL holding AX's microcode.
std::optional<SndBuild> ReadSndBuild(std::span<const uint8_t> dol);

// The main.dol of the extracted disc that holds `archive`: sys/main.dol in a folder whose files folder holds the
// archive. Dolphin and wit extract a disc that way. Nothing if there's none.
std::optional<std::filesystem::path> FindMainDol(const std::filesystem::path& archive);

} // namespace revosf2
