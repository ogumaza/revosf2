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

// LoadGame's options for revosf2: Wii games, WiiWare titles, and the sound archives inside CPK archives. Images in
// the compressed formats that ntdlib::disc_containers reads (GCZ, CISO, WIA and RVZ) are refused with a message. It
// says how to extract the disc instead.
ntdlib::disc::GameOptions DiscOptions();

// True if `input` is a game rather than a sound archive: a folder that holds a game in a layout that LoadGame reads, a
// disc image by its first bytes, or a file that LoadGame has a message for, such as a WAD (.wad) or one of a title's
// files (.tmd, .app). LoadGame reads an extracted disc, with sys/main.dol and files/ directly or in a DATA folder, and
// a title's folder, with the title's TMD (.tmd) and decrypted contents (.app). Any other folder isn't a game.
bool IsGameInput(const std::filesystem::path& input);

// The message for LoadGame's `error`. ntdlib's message for an encrypted game partition suggests an RVZ image. revosf2
// doesn't read those. Its message says how to extract the disc instead.
std::string GameMessage(const std::string& error);

// The absolute path of the game at `input`, without the separator that ends a folder given as "game/".
std::filesystem::path GamePath(const std::filesystem::path& input);

// The output folder of the game at `path` (GamePath), beside it. Its name is the folder's name, or the image's name
// without its extension, with "_revosf2" added.
std::filesystem::path GameOutputFolder(const std::filesystem::path& path);

} // namespace revosf2
