# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# The version is revosf2 --version's, and the one in the file properties of the Windows program and the Info.plist of
# the macOS app. A release changes it here and adds its notes in docs/release-notes/.
project(revosf2 VERSION 1.2.0 LANGUAGES CXX)

# seqlib writes the MIDI and SoundFont files. xsflib holds the code that the converters share with the PSF players.
# ntdlib holds the Nintendo formats and sequence_export, the conversion of their sequences. All three are submodules.
# xsflib's XsflibProject.cmake sets the C++ standard, a release build by default, the static C runtime and the compiler
# options that keep the output the same on every platform. Among them, -ffp-contract=off keeps NW4R SND's
# single-precision arithmetic without fused multiply-adds, and so the game's rounding. seqlib comes first: ntdlib's
# sequence_export uses it. ntdlib builds on xsflib and comes last.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
  message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(WARNINGS_ON_EVERY_TARGET)
set(NTDLIB_SEQUENCE_EXPORT ON)
xsflib_add_libraries(seqlib xsflib ntdlib)

# ntdlib's disc library reads games: disc images, extracted discs and WiiWare titles, and the CPK archives in them. It
# requires no other library. xsflib's rip library names each archive's folder in a game's output folder.
add_library(revosf2_core STATIC src/games.cpp src/snd_build.cpp)
target_include_directories(revosf2_core PUBLIC src)
target_link_libraries(revosf2_core PUBLIC ntdlib::disc ntdlib::rsar ntdlib::sequence_export)

add_executable(revosf2 src/main.cpp)
target_link_libraries(revosf2 PRIVATE revosf2_core xsflib::rip)
xsflib_add_version(revosf2 VERSION ${PROJECT_VERSION} DEFINE REVOSF2_VERSION
                   DESCRIPTION "Nintendo Wii sound archive converter" PRODUCT revosf2
                   COPYRIGHT "Copyright (c) 2026 the revosf2 authors. MIT licence.")

# Finder cannot drop files onto a command-line executable. Package it in a macOS droplet app, with the version in its
# Info.plist.
xsflib_add_droplet_tool(revosf2 APP_NAME Revosf2 VERSION ${PROJECT_VERSION}
                        DESCRIPTION "Drop Wii sound archives (.brsar) or JAudio start-up archives (.baa) here to convert them to MIDI, SoundFont and SFZ. Output goes in a folder beside each archive."
                        PROMPT "Choose Wii sound archives (.brsar) or JAudio start-up archives (.baa) to convert:" TRUNCATED)

include(CTest)
if(BUILD_TESTING)
  # The program reports its version, and it rejects --var 32=1. -DREVOSF2_TEST_ARCHIVE=path/to/archive.brsar also
  # converts and checks every sequence of an archive with tests/revosf2_test.cpp.
  xsflib_add_converter_tests(revosf2 EXTENSION .brsar VERSION ${PROJECT_VERSION}
                             LIBRARIES revosf2_core ntdlib::testing)
  add_test(NAME revosf2_units COMMAND revosf2_test)

  # It rejects a --dol file without AX's microcode, before it converts anything.
  add_test(NAME revosf2_dol_check COMMAND revosf2 --dol "${PROJECT_SOURCE_DIR}/LICENSE" missing.brsar)
  set_tests_properties(revosf2_dol_check PROPERTIES PASS_REGULAR_EXPRESSION "holds no AX microcode")

  # It converts a Wii game that plays its music with JAudio: from the game's start-up archive with its other sound
  # files beside it, and from an extracted disc that holds them and no sound archive. revosf2_test writes both. ctest
  # ignores the exit code of a test with a pass expression. The conversions therefore also fail if revosf2 reports a
  # failure.
  set(revosf2_jaudio ${CMAKE_CURRENT_BINARY_DIR}/jaudio)
  add_test(NAME revosf2_jaudio_files COMMAND revosf2_test --jaudio ${revosf2_jaudio})
  add_test(NAME revosf2_jaudio_archive COMMAND revosf2 ${revosf2_jaudio}/archive/test.baa)
  add_test(NAME revosf2_jaudio_game COMMAND revosf2 ${revosf2_jaudio}/disc)
  set_tests_properties(revosf2_jaudio_files PROPERTIES FIXTURES_SETUP revosf2_jaudio)
  set_tests_properties(revosf2_jaudio_archive PROPERTIES FIXTURES_REQUIRED revosf2_jaudio
                       PASS_REGULAR_EXPRESSION "test\\.baa: 1 sequences -> .*test_revosf2"
                       FAIL_REGULAR_EXPRESSION "can't read| [1-9][0-9]* failed")
  set_tests_properties(revosf2_jaudio_game PROPERTIES FIXTURES_REQUIRED revosf2_jaudio
                       PASS_REGULAR_EXPRESSION "test\\.baa: 1 sequences -> .*disc_revosf2.test"
                       FAIL_REGULAR_EXPRESSION "can't read| [1-9][0-9]* failed")

  # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
  xsflib_include_local_tests()
endif()
