// SPDX-License-Identifier: MIT

// Tests of revosf2's code: the build of NW4R SND that a main.dol shows, the search for main.dol, and the low-pass of
// the builds from November 2008. Given a sound archive (.brsar), also converts every sequence in it with the Wii's
// settings and checks each conversion. The tests of seqlib, xsflib and ntdlib hold the unit tests of the code that
// revosf2 shares with the other converters.
//
//     revosf2_test [<archive.brsar>]

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/consoles.h"
#include "ntdlib/sequence_export/filter.h"
#include "ntdlib/sequence_export/settings.h"
#include "ntdlib/testing/dol_files.h"
#include "ntdlib/testing/sequence_export_check.h"
#include "xsflib/testing/check.h"

#include "snd_build.h"

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

    XSFLIB_CHECK(old_build && old_build->October2006() && old_build->date == 0 && !old_build->Since2008());
    XSFLIB_CHECK(march && !march->October2006() && march->date == 20080308 && march->Since2008() &&
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

void TestNovember2008LowPassSteps()
{
    // The build of November 20 2008 takes the track low-pass's cutoff from a table (Util::CalcLpfFreq). A value below
    // 0.13561438 gives 80 Hz. A value from 0.9 gives 16000 Hz and turns the filter off. Between them, each thirtieth
    // moves one step up the table. Arguments 0 to 10 therefore give 80 Hz, 30 gives 640 Hz and 33 gives 1000 Hz. From
    // 58 on, the filter is off, as in the earlier builds. The build of September 19 2008 computes 1000 Hz at 32. AX
    // gets the same cutoff from both builds there. The SoundFont and SFZ cutoffs must match too.
    const revosf2::SndBuild november_build{.date = 20081120};
    const revosf2::SndBuild september_build{.date = 20080919};

    const Settings november = revosf2::SettingsFor(november_build);
    const Settings september = revosf2::SettingsFor(september_build);

    const std::span<const uint16_t> hz = november.filters->low_pass_hz;
    XSFLIB_CHECK(revosf2::SinceNovember2008(november_build) && !revosf2::SinceNovember2008(september_build));
    XSFLIB_CHECK(hz.size() == 58 && hz[0] == 80 && hz[10] == 80 && hz[11] == 100 && hz[30] == 640 && hz[32] == 800 &&
                 hz[33] == 1000 && hz[57] == 12800);
    XSFLIB_CHECK(september.filters->low_pass_hz[32] == 1000);
    XSFLIB_CHECK(TrackFilters(57, 0, 0, november).low_pass == 57 && TrackFilters(58, 0, 0, november).low_pass < 0);
    XSFLIB_CHECK(ToSoundFont(TrackFilters(33, 0, 0, november), november) ==
                 ToSoundFont(TrackFilters(32, 0, 0, september), september));
    XSFLIB_CHECK(SfzLowPassCutoff(33, november) == SfzLowPassCutoff(32, september));
}

// Convert every sequence of the archive at `path` with the settings of the build that its version gives, and check
// each conversion (ntdlib::testing::CheckArchive).
void TestArchiveConvertsEverySequence(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.empty())
    {
        std::fprintf(stderr, "can't read %s\n", path);
        failures++;
        return;
    }

    try
    {
        const ntdlib::rsar::SoundArchive archive = ntdlib::rsar::SoundArchive::Load(std::move(bytes));
        const revosf2::SndBuild build = ntdlib::sequence_export::BuildOfRsarVersion(archive.Version());
        failures += ntdlib::testing::CheckArchive(archive, revosf2::SettingsFor(build), "revosf2");
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s: %s\n", path, e.what());
        failures++;
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestMainDolShowsTheBuild();
    TestMainDolIsFoundBesideTheFiles();
    TestNovember2008LowPassSteps();

    if (argc > 1)
    {
        TestArchiveConvertsEverySequence(argv[1]);
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    std::puts("all checks passed");

    return 0;
}
