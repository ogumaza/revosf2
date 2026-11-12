// SPDX-License-Identifier: MIT

// Tests of revosf2's code: the build of NW4R SND that a main.dol shows, the settings that the sound library gives an
// archive (ArchiveSettings), the low-pass that the Wii's settings give the builds from November 2008, the profiles of
// games, the search for Mega Man 9's pieces of several sequences (ntdlib::FindSoundPieces), and which inputs are games.
// Given a sound archive (.brsar), also converts every sequence and every piece of several sequences in it with the
// Wii's settings, and checks each conversion. A main.dol after the archive gives the build of NW4R SND. With --jaudio,
// writes a small JAudio game for the tests of revosf2's JAudio inputs instead. The tests of seqlib, xsflib and ntdlib
// hold the unit tests of the code that revosf2 shares with the other converters.
//
//     revosf2_test [<archive.brsar> [<main.dol>]]
//     revosf2_test --jaudio <folder>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/disc/game.h"
#include "ntdlib/disc/header.h"
#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/archive_converter.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/convert.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/frame.h"
#include "ntdlib/sequence_export/performance.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/sound_pieces.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/rsar_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/bytes.h"
#include "xsflib/files.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

#include "games.h"
#include "profiles.h"
#include "program.h"
#include "snd_build.h"

using ntdlib::sequence_export::PerformOptions;
using ntdlib::sequence_export::RevolutionSettings;
using ntdlib::sequence_export::Settings;
using ntdlib::sequence_export::SfzLowPassCutoff;
using ntdlib::sequence_export::ToSoundFont;
using ntdlib::sequence_export::TrackFilters;
using ntdlib::testing::MicrocodeDol;
using xsflib::testing::failures;

namespace
{

// Write `bytes` to the file at `path`. A failed write fails the check.
void WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> bytes)
{
    XSFLIB_CHECK(!xsflib::WriteFile(path, bytes));
}

void TestMainDolShowsTheBuild()
{
    // AX of October 2006 transfers 0x120-byte parameter blocks. It comes with NW4R SND's build of 2006. That build has
    // no banner. A later AX transfers 0x140 bytes, and NW4R SND's banner dates the build. A build without a banner
    // plays as the build of October 2006 with a later AX too. The voices follow AX: 64 in AX of October 2006, and 96 in
    // a later AX. A file without AX's microcode shows nothing.
    const std::vector<uint8_t> kNoMicrocode(0x3000, 0);

    const std::optional<revosf2::SoundLibrary> old_build = revosf2::ReadSoundLibrary(MicrocodeDol(0x120, ""));
    const std::optional<revosf2::SoundLibrary> march = revosf2::ReadSoundLibrary(
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Mar  8 2008 10:59:21 (0x4302_145) >>"));
    const std::optional<revosf2::SoundLibrary> june = revosf2::ReadSoundLibrary(
        MicrocodeDol(0x140, "<< NW4R    - SND \tfinal   build: Jun 20 2008 11:01:02 (0x4302_158) >>"));
    const std::optional<revosf2::SoundLibrary> later_ax = revosf2::ReadSoundLibrary(MicrocodeDol(0x140, ""));
    const std::optional<revosf2::SoundLibrary> no_microcode = revosf2::ReadSoundLibrary(kNoMicrocode);
    const std::optional<revosf2::SoundLibrary> empty = revosf2::ReadSoundLibrary({});

    XSFLIB_CHECK(old_build && old_build->build.October2006() && old_build->build.date == 0 &&
                 !old_build->build.SinceMarch2008());
    XSFLIB_CHECK(march && !march->build.October2006() && march->build.date == 20080308 &&
                 march->build.SinceMarch2008() && !march->build.SinceJune2008());
    XSFLIB_CHECK(june && !june->build.October2006() && june->build.date == 20080620 && june->build.SinceJune2008());
    XSFLIB_CHECK(later_ax && later_ax->build.October2006() && later_ax->build.date == 0 &&
                 !later_ax->build.SinceMarch2008());
    XSFLIB_CHECK(!no_microcode && !empty);

    XSFLIB_CHECK(old_build && old_build->voices == 64);
    XSFLIB_CHECK(later_ax && later_ax->voices == 96);
    XSFLIB_CHECK(march && march->voices == 96);
}

void TestArchiveSettingsFollowTheLibrary()
{
    // Without a main.dol, the archive's RSAR version gives the build (BuildOfRsarVersion): a build from June 2008 for
    // version 1.4 or later, and otherwise a build of 2007. Both play 96 voices. With a main.dol, its sound library
    // gives the build and the voices. A build without a banner plays as the build of October 2006. With AX of October
    // 2006, it plays 64 voices. With a later AX, it keeps that AX's 96 voices.
    const Settings version_1_4 = revosf2::ArchiveSettings(std::nullopt, 0x0104);
    const Settings version_1_1 = revosf2::ArchiveSettings(std::nullopt, 0x0101);
    const Settings old_ax = revosf2::ArchiveSettings(revosf2::ReadSoundLibrary(MicrocodeDol(0x120, "")), 0x0104);
    const Settings later_ax = revosf2::ArchiveSettings(revosf2::ReadSoundLibrary(MicrocodeDol(0x140, "")), 0x0104);

    XSFLIB_CHECK(version_1_4.wii_build == ntdlib::sequence_export::BuildOfRsarVersion(0x0104));
    XSFLIB_CHECK(version_1_1.wii_build == ntdlib::sequence_export::BuildOfRsarVersion(0x0101));
    XSFLIB_CHECK(version_1_4.wii_build.date == 20080620 && !version_1_4.wii_build.October2006() &&
                 version_1_4.voices == 96);
    XSFLIB_CHECK(version_1_1.wii_build.date == 20070101 && !version_1_1.wii_build.October2006() &&
                 version_1_1.voices == 96);
    XSFLIB_CHECK(old_ax.wii_build.October2006() && old_ax.voices == 64);
    XSFLIB_CHECK(later_ax.wii_build.October2006() && later_ax.voices == 96);
}

// The performance that `profile` sets up for `sound` in `arrangements`. --var gives `variables`.
PerformOptions SetUp(revosf2::Profile profile, std::string_view sound, const revosf2::Arrangements& arrangements = {},
                     std::map<uint8_t, int16_t> variables = {})
{
    PerformOptions options;
    options.variables = std::move(variables);
    revosf2::SetUpSound(profile, arrangements, sound, options);

    return options;
}

void TestCityFolkSetsUpItsMusic()
{
    // Another game's main.dol selects no profile. With City Folk's profile, each hourly piece and each piece of town
    // music plays the tracks that the game plays in fine weather, and sends 0.25 to bus A. The town music leaves out
    // track 15 too. The museum's music plays the tracks of its first arrangement, without a send. K.K. Slider's songs
    // play every track and send 0.28. The profile leaves the other sounds as they are.
    const revosf2::Profile other = revosf2::ProfileOf(MicrocodeDol(0x140, ""));
    const PerformOptions hourly = SetUp(revosf2::Profile::kCityFolk, "BGM_OUTDOOR_08");
    const PerformOptions town = SetUp(revosf2::Profile::kCityFolk, "BGM_TOWN_EARLY");
    const PerformOptions museum = SetUp(revosf2::Profile::kCityFolk, "BGM_MUSEUM");
    const PerformOptions song = SetUp(revosf2::Profile::kCityFolk, "BGM_KK04_SWING");
    const PerformOptions first_town = SetUp(revosf2::Profile::kCityFolk, "BGM_TOWN_FIRST");
    const PerformOptions without = SetUp(revosf2::Profile::kNone, "BGM_OUTDOOR_08");

    const std::array<float, 3> kMusicSends = {0.25f, 0.0f, 0.0f};
    const std::array<float, 3> kSongSends = {0.28f, 0.0f, 0.0f};
    const std::array<float, 3> kNoSends = {};
    XSFLIB_CHECK(other == revosf2::Profile::kNone);
    XSFLIB_CHECK(hourly.tracks == 0x447f && hourly.fx_sends == kMusicSends);
    XSFLIB_CHECK(town.tracks == 0x19ff && town.fx_sends == kMusicSends);
    XSFLIB_CHECK(museum.tracks == 0x00e7 && museum.fx_sends == kNoSends);
    XSFLIB_CHECK(song.tracks == 0xffff && song.fx_sends == kSongSends);
    XSFLIB_CHECK(first_town.tracks == 0xffff && first_town.fx_sends == kNoSends);
    XSFLIB_CHECK(without.tracks == 0xffff && without.fx_sends == kNoSends);
}

void TestWiiPlayMutesTracks()
{
    // With Wii Play's profile, Pose Mii's music for play plays track 0 alone. Table Tennis's two pieces play the tracks
    // that the game starts them with: tracks 0 to 6, and tracks 0, 1, 2, 4, 5, 6 and 14. The profile leaves the other
    // sounds as they are, and so does another game's profile.
    XSFLIB_CHECK(SetUp(revosf2::Profile::kWiiPlay, "RP_BOM_BGM_PLAY").tracks == 0x0001);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kWiiPlay, "RP_PNP_BGM_PLAY").tracks == 0x007f);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kWiiPlay, "RP_PNP_BGM_PLAY_VS").tracks == 0x4077);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kWiiPlay, "BGM_OUTDOOR_08").tracks == 0xffff);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kCityFolk, "RP_BOM_BGM_PLAY").tracks == 0xffff);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kNone, "RP_BOM_BGM_PLAY").tracks == 0xffff);
}

void TestChannelsSetUpTheirSounds()
{
    // With the Mii Channel's profile, the music plays every track but 10 and 11, and each other sound sends 0.2 to bus
    // A. Every sound of both channels takes its tracks' sends on bus B to 0. The Mii Channel's music converts once more
    // in each of three other sets of tracks. The Wii Shop Channel leaves the tracks and bus A as the archive gives
    // them, and has no variants.
    const PerformOptions music = SetUp(revosf2::Profile::kMiiChannel, "SEQ_TMP_BGM");
    const PerformOptions effect = SetUp(revosf2::Profile::kMiiChannel, "SEQ_OK");
    const PerformOptions shop = SetUp(revosf2::Profile::kWiiShopChannel, "WIPL_BGM_SHOP");
    const std::vector<revosf2::Variant> scenes = revosf2::VariantsOf(revosf2::Profile::kMiiChannel, false);

    const std::array<float, 3> kNoBusB = {0.0f, -1.0f, 0.0f};
    const std::array<float, 3> kEffectSends = {0.2f, -1.0f, 0.0f};
    XSFLIB_CHECK(music.tracks == 0xf3ff && music.fx_sends == kNoBusB);
    XSFLIB_CHECK(effect.tracks == 0xffff && effect.fx_sends == kEffectSends);
    XSFLIB_CHECK(shop.tracks == 0xffff && shop.fx_sends == kNoBusB);
    XSFLIB_CHECK(revosf2::VariantsOf(revosf2::Profile::kWiiShopChannel, false).empty());
    XSFLIB_CHECK(scenes.size() == 3);
    if (scenes.size() == 3)
    {
        XSFLIB_CHECK(scenes[0].sound == "SEQ_TMP_BGM" && scenes[0].suffix == "_scene_4" && scenes[0].tracks == 0xf5ff);
        XSFLIB_CHECK(scenes[1].sound == "SEQ_TMP_BGM" && scenes[1].suffix == "_menu_out" && scenes[1].tracks == 0xfbff);
        XSFLIB_CHECK(scenes[2].sound == "SEQ_TMP_BGM" && scenes[2].suffix == "_scenes_1_and_3" &&
                     scenes[2].tracks == 0xf1ff);
    }
}

void TestWiiSportsSetsTheWind()
{
    // Golf's wind plays at a speed of 8 and in direction 0, unless --var gives its variables. No other sound gets a
    // variable, and the profile has no variants.
    const PerformOptions wind = SetUp(revosf2::Profile::kWiiSports, "SE_LV_Wind");
    const PerformOptions given = SetUp(revosf2::Profile::kWiiSports, "SE_LV_Wind", {}, {{0, 15}});
    const PerformOptions other = SetUp(revosf2::Profile::kWiiSports, "SE_TR_FlagHit");

    XSFLIB_CHECK((wind.variables == std::map<uint8_t, int16_t>{{0, 8}, {1, 0}}));
    XSFLIB_CHECK((given.variables == std::map<uint8_t, int16_t>{{0, 15}, {1, 0}}));
    XSFLIB_CHECK(other.variables.empty());
    XSFLIB_CHECK(revosf2::VariantsOf(revosf2::Profile::kWiiSports, false).empty());
}

void TestCityFolkPlaysTheOtherArrangements()
{
    // In rain and snow, each hourly piece and each piece of town music plays the tracks of the weather's mask, with
    // the same send. The town music leaves out track 15 in every weather. --museum N plays the tracks of the museum's
    // arrangement N. The weather leaves the museum's music and the songs as they are, and the museum's arrangement
    // leaves the other music as it is.
    const revosf2::Profile kCityFolk = revosf2::Profile::kCityFolk;
    const revosf2::Arrangements kRain = {revosf2::Weather::kRain, 1};
    const revosf2::Arrangements kSnow = {revosf2::Weather::kSnow, 1};
    const revosf2::Arrangements kMuseum4 = {revosf2::Weather::kFine, 4};
    const revosf2::Arrangements kMuseum7 = {revosf2::Weather::kFine, 7};

    const PerformOptions rain = SetUp(kCityFolk, "BGM_OUTDOOR_08", kRain);
    const PerformOptions snow = SetUp(kCityFolk, "BGM_OUTDOOR_08", kSnow);
    const PerformOptions town_rain = SetUp(kCityFolk, "BGM_TOWN_EARLY", kRain);
    const PerformOptions town_snow = SetUp(kCityFolk, "BGM_TOWN_EARLY", kSnow);
    const PerformOptions museum_in_snow = SetUp(kCityFolk, "BGM_MUSEUM", kSnow);
    const PerformOptions song_in_snow = SetUp(kCityFolk, "BGM_KK04_SWING", kSnow);
    const PerformOptions museum_4 = SetUp(kCityFolk, "BGM_MUSEUM", kMuseum4);
    const PerformOptions museum_7 = SetUp(kCityFolk, "BGM_MUSEUM", kMuseum7);
    const PerformOptions hourly_with_museum_7 = SetUp(kCityFolk, "BGM_OUTDOOR_08", kMuseum7);

    XSFLIB_CHECK(rain.tracks == 0x05c3 && rain.fx_sends[0] == 0.25f);
    XSFLIB_CHECK(snow.tracks == 0xa5c3 && snow.fx_sends[0] == 0.25f);
    XSFLIB_CHECK(town_rain.tracks == 0x4600 && town_rain.fx_sends[0] == 0.25f);
    XSFLIB_CHECK(town_snow.tracks == 0x6600 && town_snow.fx_sends[0] == 0.25f);
    XSFLIB_CHECK(museum_in_snow.tracks == 0x00e7 && museum_in_snow.fx_sends[0] == 0.0f);
    XSFLIB_CHECK(song_in_snow.tracks == 0xffff && song_in_snow.fx_sends[0] == 0.28f);
    XSFLIB_CHECK(museum_4.tracks == 0x1863 && museum_4.fx_sends[0] == 0.0f);
    XSFLIB_CHECK(museum_7.tracks == 0x0700 && museum_7.fx_sends[0] == 0.0f);
    XSFLIB_CHECK(hourly_with_museum_7.tracks == 0x447f && hourly_with_museum_7.fx_sends[0] == 0.25f);
}

void TestCityFolkStepsItsGyroids()
{
    // One frame after a gyroid's sound starts, the music takes its next step: variable 18 goes from 0 to 1. From --var
    // 18=15, the step wraps to 0. The game's count does the same. The profile changes no variable of the music, and
    // without the profile a gyroid's sound changes none.
    const auto is_step = [](const PerformOptions& options, int16_t value)
    {
        const std::vector<ntdlib::sequence_export::VariableChange>& changes = options.changes;
        return changes.size() == 1 && changes[0].frame == 1 && changes[0].index == 18 && changes[0].value == value;
    };

    const PerformOptions gyroid = SetUp(revosf2::Profile::kCityFolk, "SEQ_HNW_BCN_DKA");
    const PerformOptions last_step = SetUp(revosf2::Profile::kCityFolk, "SEQ_HNW_BCN_DKA", {}, {{18, 15}});
    const PerformOptions music = SetUp(revosf2::Profile::kCityFolk, "BGM_OUTDOOR_08");
    const PerformOptions without = SetUp(revosf2::Profile::kNone, "SEQ_HNW_BCN_DKA");

    XSFLIB_CHECK(is_step(gyroid, 1));
    XSFLIB_CHECK(is_step(last_step, 0));
    XSFLIB_CHECK(music.changes.empty());
    XSFLIB_CHECK(without.changes.empty());
}

void TestMarioKartSetsItsEffectsVariable()
{
    // With Mario Kart Wii's profile, six sound effects start with variable 0 at the value of their first play in the
    // game: 1 for SE_RC_RANK_UP, 0 for the other five. SE_CC_MSR_GATE_PASS keeps -1, the value without a variable.
    // --var gives another value. Without the profile, the effects set no variable.
    const auto first = [](std::string_view sound, int16_t value)
    {
        const std::map<uint8_t, int16_t> set = SetUp(revosf2::Profile::kMarioKart, sound).variables;
        return set.size() == 1 && set.contains(0) && set.at(0) == value;
    };

    const PerformOptions given = SetUp(revosf2::Profile::kMarioKart, "SE_RC_RANK_UP", {}, {{0, 5}});

    XSFLIB_CHECK(first("SE_CN_RBW_RING", 0));
    XSFLIB_CHECK(first("SE_CN_RBW_STAR_RING", 0));
    XSFLIB_CHECK(first("SE_RC_RANK_UP", 1));
    XSFLIB_CHECK(first("SE_RC_RANK_DOWN", 0));
    XSFLIB_CHECK(first("SE_UI_BTN_OK", 0));
    XSFLIB_CHECK(first("SE_UI_CTRL_ON", 0));
    XSFLIB_CHECK(SetUp(revosf2::Profile::kMarioKart, "SE_CC_MSR_GATE_PASS").variables.empty());
    XSFLIB_CHECK(SetUp(revosf2::Profile::kMarioKart, "SEQ_O_SELECT").variables.empty());
    XSFLIB_CHECK(given.variables.size() == 1 && given.variables.at(0) == 5);
    XSFLIB_CHECK(SetUp(revosf2::Profile::kNone, "SE_RC_RANK_UP").variables.empty());
}

void TestWeathersHaveNames()
{
    // --weather takes the names of the three weathers. Any other name is no weather.
    XSFLIB_CHECK(revosf2::WeatherNamed("fine") == revosf2::Weather::kFine);
    XSFLIB_CHECK(revosf2::WeatherNamed("rain") == revosf2::Weather::kRain);
    XSFLIB_CHECK(revosf2::WeatherNamed("snow") == revosf2::Weather::kSnow);
    XSFLIB_CHECK(!revosf2::WeatherNamed("Rain") && !revosf2::WeatherNamed("hail") && !revosf2::WeatherNamed(""));
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

    const std::vector<ntdlib::SoundPiece> pieces = ntdlib::FindSoundPieces(archive, ntdlib::kMegaMan9Channels);
    const std::vector<ntdlib::sequence_export::Sequence> sequences =
        ntdlib::sequence_export::ArchiveSequences(archive, std::span<const ntdlib::SoundPiece>(pieces));

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
    XSFLIB_CHECK(ntdlib::PieceStartingAt(pieces, 0) && !ntdlib::PieceStartingAt(pieces, 1));
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
    const xsflib::testing::TempFolder temp("revosf2_test_games");
    const std::filesystem::path& folder = temp.Path();
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
    const xsflib::testing::TempFolder temp("revosf2_test_images");
    const std::filesystem::path& folder = temp.Path();
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
    const xsflib::testing::TempFolder temp("revosf2_test_output");
    const std::filesystem::path& folder = temp.Path();
    std::filesystem::create_directories(folder / "Game");
    WriteBytes(folder / "game.nkit.iso", EncryptedWiiImage());

    const std::filesystem::path game = revosf2::GamePath(folder / "Game" / "");
    const std::filesystem::path image = revosf2::GamePath(folder / "game.nkit.iso");
    const std::filesystem::path game_output = revosf2::GameOutputFolder(game);
    const std::filesystem::path image_output = revosf2::GameOutputFolder(image);

    XSFLIB_CHECK(game.is_absolute() && game == revosf2::GamePath(folder / "Game"));
    XSFLIB_CHECK(game_output == game.parent_path() / "Game_revosf2");
    XSFLIB_CHECK(image_output == image.parent_path() / "game.nkit_revosf2");
}

// Convert each piece of several sequences in `archive` (ntdlib::FindSoundPieces) with `settings`, and check each
// conversion (ntdlib::testing::CheckConversion). Print the number of pieces and of those that loop.
void CheckPieces(const ntdlib::rsar::SoundArchive& archive, const Settings& settings)
{
    const std::vector<ntdlib::SoundPiece> pieces = ntdlib::FindSoundPieces(archive, ntdlib::kMegaMan9Channels);
    int looping = 0;
    for (const ntdlib::SoundPiece& piece : pieces)
    {
        try
        {
            const ntdlib::sequence_export::Conversion conversion = ntdlib::sequence_export::ConvertTogether(
                archive, piece.sounds, piece.name, {}, settings, revosf2::kProgram);
            failures += ntdlib::testing::CheckConversion(conversion, piece.name);
            looping += conversion.performance.loop ? 1 : 0;
            for (const std::string& warning : conversion.warnings)
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
    std::vector<uint8_t> bytes;
    if (!xsflib::ReadFile(path, bytes) || bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path);
        failures++;
        return;
    }

    std::optional<revosf2::SoundLibrary> library;
    if (dol)
    {
        std::vector<uint8_t> dol_bytes;
        if (!xsflib::ReadFile(dol, dol_bytes))
        {
            std::fprintf(stderr, "can't read %s\n", dol);
            failures++;
            return;
        }

        library = revosf2::ReadSoundLibrary(dol_bytes);
        if (!library)
        {
            std::fprintf(stderr, "%s shows no build of NW4R SND\n", dol);
            failures++;
            return;
        }
    }

    try
    {
        const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(std::move(bytes));
        const Settings settings = revosf2::ArchiveSettings(library, archive.Version());
        failures += ntdlib::testing::CheckArchive(archive, settings, revosf2::kProgram);
        CheckPieces(archive, settings);
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
        return xsflib::testing::Summary("revosf2");
    }

    TestMainDolShowsTheBuild();
    TestArchiveSettingsFollowTheLibrary();
    TestCityFolkSetsUpItsMusic();
    TestCityFolkPlaysTheOtherArrangements();
    TestWiiPlayMutesTracks();
    TestChannelsSetUpTheirSounds();
    TestWiiSportsSetsTheWind();
    TestCityFolkStepsItsGyroids();
    TestMarioKartSetsItsEffectsVariable();
    TestWeathersHaveNames();
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
