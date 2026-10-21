// SPDX-License-Identifier: MIT

// The profiles of games (see profiles.h).

#include "profiles.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "ntdlib/sequence_export/performance.h"
#include "xsflib/sha1.h"

namespace revosf2
{

namespace
{

using ntdlib::sequence_export::PerformOptions;

// Animal Crossing: City Folk (USA)'s main.dol.
constexpr std::string_view kCityFolkSha1 = "c87967b9ac0a943ba04ac16d875f5f79b4c46516";

// A piece of City Folk's music that the game plays with only some of its tracks: the tracks that sound.
struct PieceTracks
{
    std::string_view name;
    uint16_t tracks;
};

// City Folk's main.dol holds three masks of tracks for each hourly piece (BGM_OUTDOOR_00 to 23, at 0x8047e9f0) and for
// each piece of town music (BGM_TOWN_EARLY to BGM_TOWN_MIDNIGHT, at 0x8047ea90). As a piece starts, the game picks one
// by the weather (0x8007e000). It takes the first for weather 0 to 2, the second for rain (3 and 4) and the third for
// snow (5 and 6). It then sets the tracks outside the mask to volume 0 (SeqSound::SetTrackVolume, from 0x801cc490). The
// profile plays the pieces in fine weather, with the first masks. The museum's music (BGM_MUSEUM) takes a mask by room
// from the seven at 0x8047ea80. The game starts it with the first (0x801cb9bc). The rooms of stages 30 and 37 keep that
// mask.
constexpr std::array<PieceTracks, 29> kCityFolkPieces = {{
    {"BGM_OUTDOOR_00", 0x00ff}, {"BGM_OUTDOOR_01", 0x07ff}, {"BGM_OUTDOOR_02", 0x007f}, {"BGM_OUTDOOR_03", 0x03ff},
    {"BGM_OUTDOOR_04", 0x009f}, {"BGM_OUTDOOR_05", 0x002f}, {"BGM_OUTDOOR_06", 0x05ff}, {"BGM_OUTDOOR_07", 0x09ff},
    {"BGM_OUTDOOR_08", 0x447f}, {"BGM_OUTDOOR_09", 0x009f}, {"BGM_OUTDOOR_10", 0x04ff}, {"BGM_OUTDOOR_11", 0x13ff},
    {"BGM_OUTDOOR_12", 0x00ff}, {"BGM_OUTDOOR_13", 0x00ff}, {"BGM_OUTDOOR_14", 0x03ff}, {"BGM_OUTDOOR_15", 0x007f},
    {"BGM_OUTDOOR_16", 0x07ff}, {"BGM_OUTDOOR_17", 0x02ff}, {"BGM_OUTDOOR_18", 0x077f}, {"BGM_OUTDOOR_19", 0x01ff},
    {"BGM_OUTDOOR_20", 0x007f}, {"BGM_OUTDOOR_21", 0x1f7f}, {"BGM_OUTDOOR_22", 0x19ff}, {"BGM_OUTDOOR_23", 0x003f},
    {"BGM_TOWN_EARLY", 0x99ff}, {"BGM_TOWN_DAY", 0x07ff},   {"BGM_TOWN_NIGHT", 0x01ff}, {"BGM_TOWN_MIDNIGHT", 0x003f},
    {"BGM_MUSEUM", 0x00e7},
}};

// The start of the town music also sets its track 15 to volume 0 (0x801cbc34). Only code that fades the track by the
// distance to a source (0x801cbec8) raises it again. Nothing in the game calls the functions that turn that code on
// (0x8007a778 and 0x80079294).
constexpr uint16_t kTownTrack15 = 0x8000;

// The start of the music (0x801cbbbc) sets the sound's send to bus A, where the game's reverb is. The hourly and town
// music send 0.25. K.K. Slider's songs, sounds 0x1a29 to 0x1a77, send 0.28. The names of those songs start with BGM_KK.
constexpr float kMusicSend = 0.25f;
constexpr float kSongSend = 0.28f;

void SetUpCityFolkSound(std::string_view sound, PerformOptions& perform)
{
    if (sound.starts_with("BGM_KK"))
    {
        perform.fx_sends[0] = kSongSend;
        return;
    }

    for (const PieceTracks& piece : kCityFolkPieces)
    {
        if (piece.name != sound)
        {
            continue;
        }

        const bool town = sound.starts_with("BGM_TOWN_");
        perform.tracks = town ? static_cast<uint16_t>(piece.tracks & ~kTownTrack15) : piece.tracks;

        if (sound != "BGM_MUSEUM")
        {
            perform.fx_sends[0] = kMusicSend;
        }

        return;
    }
}

} // namespace

Profile ProfileOf(std::span<const uint8_t> dol)
{
    return xsflib::Sha1::Hex(xsflib::Sha1::Of(dol)) == kCityFolkSha1 ? Profile::kCityFolk : Profile::kNone;
}

void SetUpSound(Profile profile, std::string_view sound, PerformOptions& perform)
{
    if (profile == Profile::kCityFolk)
    {
        SetUpCityFolkSound(sound, perform);
    }
}

} // namespace revosf2
