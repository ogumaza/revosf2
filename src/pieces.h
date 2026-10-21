// SPDX-License-Identifier: MIT

// Music that a game plays as several sequences together. A game can start several sequences in one sound frame and play
// them as one piece. revosf2 converts such a piece to one MIDI file, one SoundFont and one set of SFZ files (ntdlib's
// ConvertTogether).
//
// Mega Man 9 plays each piece of its music as four sequences, one for each channel of the NES's sound chip: the two
// pulse channels, the triangle and the noise. Their labels are one name followed by "_S1", "_S2", "_TR" and "_NO". The
// game's table of its 33 pieces (0x802805c0) gives each piece's four sounds in that order. The function at 0x800d5564
// starts the four in that order, with one call for each. The four start in the same sound frame.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ntdlib/rsar/formats.h"
#include "ntdlib/sequence_export/frame.h"

namespace revosf2
{

// A piece that a game plays as several sequences: its name, and its sequences' sounds, in the order in which the game
// starts them.
struct Piece
{
    std::string name;
    std::vector<uint32_t> sounds;
};

// The pieces of `archive`, in the order of their first sounds. A piece's sequences have the labels of Mega Man 9's
// music: one name followed by "_S1", "_S2", "_TR" and "_NO". The piece takes that name. Of two sequences with one
// label, the first counts.
std::vector<Piece> FindPieces(const ntdlib::rsar::SoundArchive& archive);

// The piece of `pieces` whose first sound is `index`, or none.
std::optional<Piece> PieceAt(const std::vector<Piece>& pieces, std::size_t index);

// The sequences of `archive` for the frame, in the order of its sounds. A piece takes the place of its first sequence,
// with the piece's name and its sequences' labels. Its other sequences leave the list.
std::vector<ntdlib::sequence_export::Sequence> ArchiveSequences(const ntdlib::rsar::SoundArchive& archive);

} // namespace revosf2
