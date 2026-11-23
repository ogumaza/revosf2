// SPDX-License-Identifier: MIT

// Games as inputs (see games.h).

#include "games.h"

#include <filesystem>
#include <string>
#include <string_view>

#include "ntdlib/disc/game.h"
#include "seqlib/file_name.h"
#include "xsflib/files.h"
#include "xsflib/rip/tool.h"

#include "program.h"

namespace revosf2
{

namespace fs = std::filesystem;

namespace
{

// The start of ntdlib's message for a Wii image whose game partition is encrypted.
constexpr std::string_view kEncrypted = "the game partition is encrypted.";

// How to get a game's files from an image that revosf2 can't read.
constexpr std::string_view kExtract =
    "Extract the disc with Dolphin: right-click the game and choose Properties. On the Filesystem tab, right-click the "
    "disc and choose Extract Entire Disc. Then give revosf2 the folder that Dolphin extracted it to";

} // namespace

ntdlib::disc::GameOptions DiscOptions()
{
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kWii, kProgram};
    options.image.refuse_container = [](const std::string& format)
    {
        return "it's " + std::string(format == "RVZ" ? "an " : "a ") + format + " disc image. " + kProgram +
               " can't read those. " + std::string(kExtract);
    };

    options.titles = true;
    options.cpk_sound_archives = true;

    return options;
}

bool IsGameInput(const fs::path& input)
{
    return ntdlib::disc::IsGame(input, DiscOptions());
}

std::string GameMessage(const std::string& error)
{
    if (!error.starts_with(kEncrypted))
    {
        return error;
    }

    return std::string(kEncrypted) + " " + kProgram +
           " reads only decrypted partitions. NKit images store them that way. " + std::string(kExtract);
}

fs::path GameOutputFolder(const fs::path& path)
{
    return path.parent_path() /
           xsflib::PathFromUtf8(seqlib::SafeFileName(xsflib::rip::NameOfInput(xsflib::Utf8(path)) + "_" + kProgram));
}

} // namespace revosf2
