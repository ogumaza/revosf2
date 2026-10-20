// SPDX-License-Identifier: MIT

// Tests of revosf2's code: the build of NW4R SND that a main.dol shows, the search for main.dol, the low-pass that the
// Wii's settings give the builds from November 2008, the profiles of games, the pieces of several sequences, and which
// inputs are games. Given a sound archive (.brsar), also converts every sequence and every piece of several sequences
// in it with the Wii's settings, and checks each conversion. A main.dol after the archive gives the build of NW4R SND.
// With --jaudio, writes a small JAudio game for the tests of revosf2's JAudio inputs instead. The tests of seqlib,
// xsflib and ntdlib hold the unit tests of the code that revosf2 shares with the other converters.
//
//     revosf2_test [<archive.brsar> [<main.dol>]]
//     revosf2_test --jaudio <folder>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/header.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "games.h"
#include "pieces.h"
#include "profiles.h"
#include "snd_build.h"

using ntdlib::sequence_export::RevolutionSettings;
using ntdlib::sequence_export::Settings;
using ntdlib::sequence_export::SfzLowPassCutoff;
using ntdlib::sequence_export::ToSoundFont;
using ntdlib::sequence_export::TrackFilters;
using ntdlib::testing::MicrocodeDol;
using xsflib::testing::failures;

namespace
{

void TestMainDolShowsTheBuild()
{
    // AX of October 2006 transfers 0x120-byte parameter blocks. It comes with NW4R SND's build of 2006. That build has
    // no banner. A later AX transfers 0x140 bytes, and NW4R SND's banner dates the build. A file without AX's microcode
    // shows nothing.
    const std::vector<uint8_t> kNoMicrocode(0x3000, 0);

    const std::optional<revosf2::SndBuild> old_build = revosf2::ReadSndBuild(MicrocodeDol(0x120, ""));
    const std::optional<revosf2::SndBuild> march = revosf2::ReadSndBuild(
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Mar  8 2008 10:59:21 (0x4302_145) >>"));
    const std::optional<revosf2::SndBuild> june = revosf2::ReadSndBuild(
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Jun 20 2008 11:01:02 (0x4302_158) >>"));
    const std::optional<revosf2::SndBuild> no_microcode = revosf2::ReadSndBuild(kNoMicrocode);
    const std::optional<revosf2::SndBuild> empty = revosf2::ReadSndBuild({});

    XSFLIB_CHECK(old_build && old_build->October2006() && old_build->date == 0 && !old_build->SinceMarch2008());
    XSFLIB_CHECK(march && !march->October2006() && march->date == 20080308 && march->SinceMarch2008() &&
                 !march->SinceJune2008());
    XSFLIB_CHECK(june && !june->October2006() && june->date == 20080620 && june->SinceJune2008());
    XSFLIB_CHECK(!no_microcode && !empty);
}

void TestMainDolIsFoundBesideTheFiles()
{
    // Dolphin and wit extract a disc into a sys folder and a files folder. An archive anywhere in files finds
    // sys/main.dol. An archive outside a files folder finds nothing.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "revosf2_test_disc";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "files" / "Sound");
    std::filesystem::create_directories(root / "sys");
    for (const std::filesystem::path& file :
         {root / "sys" / "main.dol", root / "files" / "Sound" / "a.brsar", root / "a.brsar"})
    {
        std::ofstream(file, std::ios::binary).put('\0');
    }

    const std::optional<std::filesystem::path> inside = revosf2::FindMainDol(root / "files" / "Sound" / "a.brsar");
    const std::optional<std::filesystem::path> outside = revosf2::FindMainDol(root / "a.brsar");
    std::filesystem::remove_all(root);

    XSFLIB_CHECK(inside && inside->filename() == "main.dol" && inside->parent_path().filename() == "sys");
    XSFLIB_CHECK(!outside);
}

void TestCityFolkSetsUpItsMusic()
{
    // Another game's main.dol selects no profile. With City Folk's profile, each hourly piece and each piece of town
    // music plays the tracks that the game plays in fine weather, and sends 0.25 to bus A. The town music leaves out
    // track 15 too. The museum's music plays the tracks of its first room, without a send. K.K. Slider's songs play
    // every track and send 0.28. The profile leaves the other sounds as they are.
    const auto set_up = [](revosf2::Profile profile, std::string_view sound)
    {
        ntdlib::sequence_export::PerformOptions options;
        revosf2::SetUpSound(profile, sound, options);
        return std::pair(options.tracks, options.fx_sends);
    };
    const std::array<float, 3> kNoSends = {};

    const revosf2::Profile other = revosf2::ProfileOf(MicrocodeDol(0x140, ""));
    const auto hourly = set_up(revosf2::Profile::kCityFolk, "BGM_OUTDOOR_08");
    const auto town = set_up(revosf2::Profile::kCityFolk, "BGM_TOWN_EARLY");
    const auto museum = set_up(revosf2::Profile::kCityFolk, "BGM_MUSEUM");
    const auto song = set_up(revosf2::Profile::kCityFolk, "BGM_KK04_SWING");
    const auto first_town = set_up(revosf2::Profile::kCityFolk, "BGM_TOWN_FIRST");
    const auto without = set_up(revosf2::Profile::kNone, "BGM_OUTDOOR_08");

    XSFLIB_CHECK(other == revosf2::Profile::kNone);
    XSFLIB_CHECK(hourly == std::pair(uint16_t{0x447f}, std::array<float, 3>{0.25f, 0.0f, 0.0f}));
    XSFLIB_CHECK(town == std::pair(uint16_t{0x19ff}, std::array<float, 3>{0.25f, 0.0f, 0.0f}));
    XSFLIB_CHECK(museum == std::pair(uint16_t{0x00e7}, kNoSends));
    XSFLIB_CHECK(song == std::pair(uint16_t{0xffff}, std::array<float, 3>{0.28f, 0.0f, 0.0f}));
    XSFLIB_CHECK(first_town == std::pair(uint16_t{0xffff}, kNoSends));
    XSFLIB_CHECK(without == std::pair(uint16_t{0xffff}, kNoSends));
}

void TestPiecesAreFoundByTheirLabels()
{
    // Four sequences whose labels are one name followed by _S1, _S2, _TR and _NO make a piece. It takes the name and
    // the place of its first sequence in the list. Three of the four labels make no piece. A stream with the first
    // label doesn't start one. Neither does a label that is only the first ending.
    const std::vector<uint8_t> bytes = ntdlib::testing::rsar_files::MakeSoundTable({{"SEQ_TITLE_S1", 1, 0},
                                                                                    {"SEQ_TITLE_S2", 1, 0},
                                                                                    {"SEQ_TITLE_TR", 1, 0},
                                                                                    {"SEQ_TITLE_NO", 1, 0},
                                                                                    {"SEQ_JINGLE", 1, 0},
                                                                                    {"SEQ_SHORT_S1", 1, 0},
                                                                                    {"SEQ_SHORT_S2", 1, 0},
                                                                                    {"SEQ_SHORT_TR", 1, 0},
                                                                                    {"STRM_S1", 2, 0},
                                                                                    {"STRM_S2", 1, 0},
                                                                                    {"STRM_TR", 1, 0},
                                                                                    {"STRM_NO", 1, 0},
                                                                                    {"_S1", 1, 0}},
                                                                                   {"PLAYER_BGM"});
    const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(bytes);

    const std::vector<revosf2::Piece> pieces = revosf2::FindPieces(archive);
    const std::vector<ntdlib::sequence_export::Sequence> sequences = revosf2::ArchiveSequences(archive);

    std::vector<std::string> names;
    for (const ntdlib::sequence_export::Sequence& sequence : sequences)
    {
        names.push_back(sequence.name);
    }
    const std::vector<std::string> kNames = {"SEQ_TITLE", "SEQ_JINGLE", "SEQ_SHORT_S1", "SEQ_SHORT_S2", "SEQ_SHORT_TR",
                                             "STRM_S2",   "STRM_TR",    "STRM_NO",      "_S1"};
    const std::vector<std::string> kLabels = {"SEQ_TITLE_S1", "SEQ_TITLE_S2", "SEQ_TITLE_TR", "SEQ_TITLE_NO"};
    XSFLIB_CHECK(pieces.size() == 1 && pieces[0].name == "SEQ_TITLE" &&
                 pieces[0].sounds == (std::vector<uint32_t>{0, 1, 2, 3}));
    XSFLIB_CHECK(names == kNames && sequences[0].index == 0 && sequences[0].labels == kLabels &&
                 sequences[1].labels.empty());
    XSFLIB_CHECK(revosf2::PieceAt(pieces, 0) && !revosf2::PieceAt(pieces, 1));
}

void TestNovember2008LowPassSteps()
{
    // The build of November 20 2008 takes the track low-pass's cutoff from a table (Util::CalcLpfFreq). A value below
    // 0.13561438 gives 80 Hz. A value from 0.9 gives 16000 Hz and turns the filter off. Between them, each thirtieth
    // moves one step up the table. Arguments 0 to 10 therefore give 80 Hz, 30 gives 640 Hz and 33 gives 1000 Hz. From
    // 58 on, the filter is off, as in the earlier builds. The build of September 19 2008 computes 1000 Hz at 32. AX
    // gets the same cutoff from both builds there. The SoundFont and SFZ cutoffs must match too.
    const revosf2::SndBuild november_build{.date = 20081120};
    const revosf2::SndBuild september_build{.date = 20080919};

    const Settings november = RevolutionSettings(november_build);
    const Settings september = RevolutionSettings(september_build);

    const std::span<const uint16_t> hz = november.filters->low_pass_hz;
    XSFLIB_CHECK(november_build.SinceNovember2008() && !september_build.SinceNovember2008());
    XSFLIB_CHECK(hz.size() == 58 && hz[0] == 80 && hz[10] == 80 && hz[11] == 100 && hz[30] == 640 && hz[32] == 800 &&
                 hz[33] == 1000 && hz[57] == 12800);
    XSFLIB_CHECK(september.filters->low_pass_hz[32] == 1000);
    XSFLIB_CHECK(TrackFilters(57, 0, 0, november).low_pass == 57 && TrackFilters(58, 0, 0, november).low_pass < 0);
    XSFLIB_CHECK(ToSoundFont(TrackFilters(33, 0, 0, november), november) ==
                 ToSoundFont(TrackFilters(32, 0, 0, september), september));
    XSFLIB_CHECK(SfzLowPassCutoff(33, november) == SfzLowPassCutoff(32, september));
}

// An empty folder in the system's temp folder, for one test. Two runs at once get different folders.
std::filesystem::path TempFolder(const std::string& name)
{
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / (name + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);

    return folder;
}

void WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A Wii disc image whose game partition is encrypted. A dump of a disc stores the partition that way. The image holds
// the disc header, a partition table that lists the game partition, and the partition's header (WiiBrew's "Wii disc").
// The partition's data doesn't start with a disc header. ntdlib reads zeros past an image's end. The image therefore
// ends with the partition's header.
std::vector<uint8_t> EncryptedWiiImage()
{
    constexpr std::size_t kTable = 0x40000;
    constexpr std::size_t kEntries = 0x40020;
    constexpr std::size_t kPartition = 0x50000;
    std::vector<uint8_t> image(kPartition + 0x2C0);
    xsflib::StoreBe32(image.data() + ntdlib::disc::kWiiMagicOffset, ntdlib::disc::kWiiMagic);

    // The table's first group holds one partition, at kPartition. Its type, 0, makes it the game partition. The table
    // gives offsets divided by 4.
    xsflib::StoreBe32(image.data() + kTable, 1);
    xsflib::StoreBe32(image.data() + kTable + 4, kEntries >> 2);
    xsflib::StoreBe32(image.data() + kEntries, kPartition >> 2);

    // The partition's header gives the TMD's size and offset, and the offset of the partition's data. The offsets are
    // divided by 4 too.
    xsflib::StoreBe32(image.data() + kPartition + 0x2A4, 0x20);
    xsflib::StoreBe32(image.data() + kPartition + 0x2A8, 0x2C0 >> 2);
    xsflib::StoreBe32(image.data() + kPartition + 0x2B8, 0x20000 >> 2);

    return image;
}

void TestGamesAreFoldersImagesAndTitleFiles()
{
    // A folder is a game if it holds an extracted disc, with sys/main.dol directly or in a DATA folder, or a title's
    // TMD or contents, whatever the case of their extensions. Another folder isn't, and neither is one whose only title
    // file is an AppleDouble file, or a folder named like a content. A disc image is a game, by its first bytes. So is
    // a file that LoadGame has a message for, such as a WAD. A sound archive isn't, and neither is a missing file.
    const std::filesystem::path folder = TempFolder("revosf2_test_games");
    for (const std::filesystem::path& subfolder :
         {folder / "disc" / "sys", folder / "extracted" / "DATA" / "sys", folder / "title", folder / "other",
          folder / "apple_double", folder / "bundle" / "a.app"})
    {
        std::filesystem::create_directories(subfolder);
    }
    const std::vector<uint8_t> kArchive = {'R', 'S', 'A', 'R', 0xFE, 0xFF, 0x01, 0x04};
    const std::vector<uint8_t> kRvz = {'R', 'V', 'Z', 0x01};
    WriteBytes(folder / "disc" / "sys" / "main.dol", kArchive);
    WriteBytes(folder / "extracted" / "DATA" / "sys" / "main.dol", kArchive);
    WriteBytes(folder / "title" / "title.TMD", kArchive);
    WriteBytes(folder / "other" / "a.brsar", kArchive);
    WriteBytes(folder / "apple_double" / "._content.app", kArchive);
    WriteBytes(folder / "a.brsar", kArchive);
    WriteBytes(folder / "image.bin", kRvz);
    WriteBytes(folder / "game.iso", EncryptedWiiImage());
    WriteBytes(folder / "title.WAD", kArchive);

    const bool disc = revosf2::IsGameInput(folder / "disc");
    const bool extracted = revosf2::IsGameInput(folder / "extracted");
    const bool title = revosf2::IsGameInput(folder / "title");
    const bool other = revosf2::IsGameInput(folder / "other");
    const bool apple_double = revosf2::IsGameInput(folder / "apple_double");
    const bool bundle = revosf2::IsGameInput(folder / "bundle");
    const bool rvz = revosf2::IsGameInput(folder / "image.bin");
    const bool iso = revosf2::IsGameInput(folder / "game.iso");
    const bool wad = revosf2::IsGameInput(folder / "title.WAD");
    const bool archive = revosf2::IsGameInput(folder / "a.brsar");
    const bool missing = revosf2::IsGameInput(folder / "missing.iso");
    std::filesystem::remove_all(folder);

    XSFLIB_CHECK(disc && extracted && title);
    XSFLIB_CHECK(!other && !apple_double && !bundle);
    XSFLIB_CHECK(rvz && iso && wad);
    XSFLIB_CHECK(!archive && !missing);
}

void TestImagesThatRevosf2CantRead()
{
    // revosf2 reads Wii games, titles and the sound archives in CPKs. It reads no compressed image. Its message for one
    // gives the steps to extract the disc instead. ntdlib's message for an encrypted partition suggests an RVZ image.
    // revosf2's message there gives the same steps. A WAD gets ntdlib's message.
    const std::filesystem::path folder = TempFolder("revosf2_test_images");
    const std::vector<uint8_t> kRvz = {'R', 'V', 'Z', 0x01};
    WriteBytes(folder / "game.rvz", kRvz);
    WriteBytes(folder / "game.iso", EncryptedWiiImage());
    WriteBytes(folder / "title.wad", kRvz);
    const ntdlib::disc::GameOptions options = revosf2::DiscOptions();

    ntdlib::disc::GameFiles game;
    const std::optional<std::string> rvz = ntdlib::disc::LoadGame(folder / "game.rvz", game, options);
    const std::optional<std::string> encrypted = ntdlib::disc::LoadGame(folder / "game.iso", game, options);
    const std::optional<std::string> wad = ntdlib::disc::LoadGame(folder / "title.wad", game, options);
    const std::string encrypted_message = revosf2::GameMessage(encrypted.value_or(""));
    std::filesystem::remove_all(folder);

    XSFLIB_CHECK(options.console == ntdlib::disc::Console::kWii && options.titles && options.cpk_sound_archives);
    XSFLIB_CHECK(rvz && rvz->starts_with("it's an RVZ disc image. revosf2 can't read those. Extract the disc") &&
                 rvz->find("Extract Entire Disc") != std::string::npos);
    XSFLIB_CHECK(encrypted_message.starts_with("the game partition is encrypted. revosf2 reads only decrypted") &&
                 encrypted_message.find("Extract Entire Disc") != std::string::npos &&
                 encrypted_message.find("RVZ") == std::string::npos);
    XSFLIB_CHECK(wad && wad->find("decrypted contents (.app)") != std::string::npos);
    XSFLIB_CHECK(revosf2::GameMessage("the disc has no game partition") == "the disc has no game partition");
}

void TestGameOutputFolders()
{
    // A game's output folder is beside the game. It takes the name of the game's folder, or of the image without its
    // extension. A folder given with a separator at its end is the same game.
    const std::filesystem::path folder = TempFolder("revosf2_test_output");
    std::filesystem::create_directories(folder / "Game");
    WriteBytes(folder / "game.nkit.iso", EncryptedWiiImage());

    const std::filesystem::path game = revosf2::GamePath(folder / "Game" / "");
    const std::filesystem::path image = revosf2::GamePath(folder / "game.nkit.iso");
    const std::filesystem::path game_output = revosf2::GameOutputFolder(game);
    const std::filesystem::path image_output = revosf2::GameOutputFolder(image);
    std::filesystem::remove_all(folder);

    XSFLIB_CHECK(game.is_absolute() && game == revosf2::GamePath(folder / "Game"));
    XSFLIB_CHECK(game_output == game.parent_path() / "Game_revosf2");
    XSFLIB_CHECK(image_output == image.parent_path() / "game.nkit_revosf2");
}

// Convert each piece of several sequences in `archive` (FindPieces) with `settings`, and check each conversion
// (ntdlib::testing::CheckConversion). Print the number of pieces and of those that loop.
void CheckPieces(const ntdlib::rsar::SoundArchive& archive, const Settings& settings)
{
    const std::vector<revosf2::Piece> pieces = revosf2::FindPieces(archive);
    int looping = 0;
    for (const revosf2::Piece& piece : pieces)
    {
        try
        {
            const ntdlib::sequence_export::Conversion c =
                ntdlib::sequence_export::ConvertTogether(archive, piece.sounds, piece.name, {}, settings, "revosf2");
            failures += ntdlib::testing::CheckConversion(c, piece.name);
            looping += c.performance.loop ? 1 : 0;
            for (const std::string& warning : c.warnings)
            {
                std::fprintf(stderr, "%s: %s\n", piece.name.c_str(), warning.c_str());
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "%s: %s\n", piece.name.c_str(), e.what());
            failures++;
        }
    }

    std::printf("archive: %zu pieces, %d loop\n", pieces.size(), looping);
}

// Convert every sequence and every piece of several sequences of the archive at `path`, and check each conversion
// (ntdlib::testing::CheckArchive and CheckPieces). The main.dol at `dol` gives the build of NW4R SND. Without one, the
// archive's version gives it.
void TestArchiveConvertsEverySequence(const char* path, const char* dol)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path);
        failures++;
        return;
    }

    std::optional<revosf2::SndBuild> dol_build;
    if (dol)
    {
        std::ifstream dol_file(dol, std::ios::binary);
        const std::vector<uint8_t> dol_bytes((std::istreambuf_iterator<char>(dol_file)), {});
        dol_build = revosf2::ReadSndBuild(dol_bytes);
        if (!dol_build)
        {
            std::fprintf(stderr, "%s shows no build of NW4R SND\n", dol);
            failures++;
            return;
        }
    }

    try
    {
        const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(std::move(bytes));
        const revosf2::SndBuild build =
            dol_build.value_or(ntdlib::sequence_export::BuildOfRsarVersion(archive.Version()));
        failures += ntdlib::testing::CheckArchive(archive, RevolutionSettings(build), "revosf2");
        CheckPieces(archive, RevolutionSettings(build));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
    }
}

// Write a small JAudio game's sound files twice in `folder` (ntdlib's WriteGameFiles): loose in "archive", and in an
// extracted disc, "disc", whose files/Audiores folder holds them. The disc has no sound archive (.brsar). Its sys
// folder holds the files that LoadGame requires: a header with the Wii's magic word, a main.dol and an empty file
// system table.
void WriteJAudioGames(const std::filesystem::path& folder)
{
    std::filesystem::remove_all(folder);
    ntdlib::testing::jaudio_files::WriteGameFiles(folder / "archive");
    ntdlib::testing::jaudio_files::WriteGameFiles(folder / "disc" / "files" / "Audiores");
    std::vector<uint8_t> header(ntdlib::disc::kHeaderSize);
    xsflib::StoreBe32(header.data() + ntdlib::disc::kWiiMagicOffset, ntdlib::disc::kWiiMagic);
    const std::vector<uint8_t> kDol = {0x00, 0x00, 0x01, 0x00};
    const std::vector<uint8_t> kFst(12);
    std::filesystem::create_directories(folder / "disc" / "sys");
    WriteBytes(folder / "disc" / "sys" / "boot.bin", header);
    WriteBytes(folder / "disc" / "sys" / "main.dol", kDol);
    WriteBytes(folder / "disc" / "sys" / "fst.bin", kFst);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == "--jaudio")
    {
        WriteJAudioGames(argv[2]);
        return 0;
    }

    TestMainDolShowsTheBuild();
    TestMainDolIsFoundBesideTheFiles();
    TestCityFolkSetsUpItsMusic();
    TestPiecesAreFoundByTheirLabels();
    TestNovember2008LowPassSteps();
    TestGamesAreFoldersImagesAndTitleFiles();
    TestImagesThatRevosf2CantRead();
    TestGameOutputFolders();

    if (argc > 1)
    {
        TestArchiveConvertsEverySequence(argv[1], argc > 2 ? argv[2] : nullptr);
    }

    return xsflib::testing::Summary("revosf2");
}
