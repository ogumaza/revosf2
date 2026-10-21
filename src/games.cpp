// SPDX-License-Identifier: MIT

// Games as inputs (see games.h).

#include "games.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/image.h"
#include "seqlib/file_name.h"
#include "xsflib/ascii.h"
#include "xsflib/files.h"

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

// True if `folder` holds a game in a layout that LoadGame reads (see IsGameInput). LoadGame finds a title's files by
// their extensions, in any case, and leaves out AppleDouble files ("._" and a file's name).
bool HoldsGame(const fs::path& folder)
{
    std::error_code error;
    if (fs::is_regular_file(folder / "sys" / "main.dol", error) ||
        fs::is_regular_file(folder / "DATA" / "sys" / "main.dol", error))
    {
        return true;
    }

    for (auto it = fs::directory_iterator(folder, error); !error && it != fs::directory_iterator(); it.increment(error))
    {
        std::error_code type_error;
        const std::string name = xsflib::AsciiLower(xsflib::Utf8(it->path().filename()));
        if ((name.ends_with(".tmd") || name.ends_with(".app")) && !name.starts_with("._") &&
            it->is_regular_file(type_error))
        {
            return true;
        }
    }

    return false;
}

} // namespace

ntdlib::disc::GameOptions DiscOptions()
{
    ntdlib::disc::GameOptions options{ntdlib::disc::Console::kWii, "revosf2"};
    options.image.refuse_container = [](const std::string& format)
    {
        return "it's " + std::string(format == "RVZ" ? "an " : "a ") + format +
               " disc image. revosf2 can't read those. " + std::string(kExtract);
    };

    options.titles = true;
    options.cpk_sound_archives = true;

    return options;
}

bool IsGameInput(const fs::path& input)
{
    std::error_code error;
    if (fs::is_directory(input, error))
    {
        return HoldsGame(input);
    }

    const std::string extension = xsflib::AsciiLower(xsflib::Utf8(input.extension()));
    if (extension == ".wad" || extension == ".tmd" || extension == ".app")
    {
        return true;
    }

    // A disc image shows in its first 0x20 bytes. A file that can't be read stays an archive for the frame to report.
    std::vector<uint8_t> head(0x20);
    std::ifstream file(input, std::ios::binary);
    file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(file.gcount()));

    return ntdlib::disc::ImageFormat(head).has_value();
}

std::string GameMessage(const std::string& error)
{
    if (!error.starts_with(kEncrypted))
    {
        return error;
    }

    return std::string(kEncrypted) + " revosf2 reads only decrypted partitions. NKit images store them that way. " +
           std::string(kExtract);
}

fs::path GamePath(const fs::path& input)
{
    std::error_code error;
    fs::path path = fs::absolute(input, error);
    if (error)
    {
        path = input;
    }

    path = path.lexically_normal();

    return path.has_filename() ? path : path.parent_path();
}

fs::path GameOutputFolder(const fs::path& path)
{
    std::error_code error;
    const fs::path name = fs::is_directory(path, error) ? path.filename() : path.stem();

    return path.parent_path() / xsflib::PathFromUtf8(seqlib::SafeFileName(xsflib::Utf8(name) + "_revosf2"));
}

} // namespace revosf2
