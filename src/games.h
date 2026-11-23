// SPDX-License-Identifier: MIT

// Games as inputs: a Wii disc image, an extracted disc, or a WiiWare title's folder with the title's TMD and decrypted
// contents. ntdlib's LoadGame reads them. It also lists the sound archives inside the game's CRI CPK archives. Each
// sound archive of a game converts the way a loose archive does. Its files go in a folder of its own in the game's
// output folder.

#pragma once

#include <filesystem>
#include <string>

#include "ntdlib/disc/game.h"

namespace revosf2
{

// LoadGame's options for revosf2: Wii games, WiiWare titles, and the sound archives inside CPK archives. Images in the
// compressed formats that ntdlib::disc_containers reads (GCZ, CISO, WIA and RVZ) are refused with a message. It says
// how to extract the disc instead.
ntdlib::disc::GameOptions DiscOptions();

// True if `input` is a game rather than a sound archive, by LoadGame's rules with DiscOptions (ntdlib::disc::IsGame): a
// folder that contains an extracted disc or a title's TMD or contents, a disc image by its first bytes, or a file that
// LoadGame has a message for, such as a WAD or one of a title's files. Any other folder isn't a game.
bool IsGameInput(const std::filesystem::path& input);

// The message for LoadGame's `error`. ntdlib's message for an encrypted game partition suggests an RVZ image. revosf2
// doesn't read those. Its message says how to extract the disc instead.
std::string GameMessage(const std::string& error);

// The output folder of the game at `path`, beside it. `path` is xsflib's rip::InputPath of the input. The folder's name
// is the game folder's name, or the image's name without its extension (rip::NameOfInput), with "_revosf2" added.
std::filesystem::path GameOutputFolder(const std::filesystem::path& path);

} // namespace revosf2
