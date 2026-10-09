// SPDX-License-Identifier: MIT

// The bank, instruments and decoded waves a sound uses.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rsar.h"
#include "wave.h"

namespace revosf2
{

// The keys and velocities with which one region plays. NW4R SND selects a key's entry in an instrument's key table,
// then a velocity's entry in that entry's velocity table. A region's notes are then a set of keys that select the same
// entry, times a set of velocities.
struct PlayedRanges
{
    const Region* region = nullptr;
    std::vector<std::pair<int, int>> keys, velocities;
};

// Return the regions of `program` in `bank` with the keys and velocities that play them, in key order. Keys and
// velocities that play nothing are left out.
std::vector<PlayedRanges> RegionRanges(const Bank& bank, uint32_t program);

// A sequence's bank, with its waves decoded on demand. Read failures are collected in Errors() and silence only the
// affected notes.
class BankSet
{
public:
    BankSet(const SoundArchive& archive, const SoundInfo& sound);

    // Return the bank, or nullptr if it can't be read.
    const Bank* GetBank() const
    {
        return bank_ ? &*bank_ : nullptr;
    }

    // Return the bank name, BANK_<index> for unnamed banks, or an empty string if the sound names none.
    const std::string& BankName() const
    {
        return bank_name_;
    }

    // The velocity region that plays `key` at `velocity` on `program`, or nullptr.
    const Region* FindRegion(uint32_t program, int key, int velocity) const;

    // True if the bank has an instrument for `program`, though it may play nothing.
    bool HasInstrument(uint32_t program) const;

    // Decode and cache bank wave `wave_index`, or return nullptr. Errors are added to Errors().
    const Pcm* GetWave(uint32_t wave_index);

    // Identify a wave across the archive by (bank index, wave index).
    std::pair<uint32_t, uint32_t> WaveKey(uint32_t wave_index) const
    {
        return {bank_index_, wave_index};
    }

    // Read failures for the bank and its waves.
    const std::vector<std::string>& Errors() const
    {
        return errors_;
    }

private:
    // Load the bank's waves on first use: the RWAR of a later bank, or nothing for a version 1.1 bank, whose WAVE block
    // describes them. Report what's missing.
    void LoadWaves();

    const SoundArchive& archive_;
    uint32_t bank_index_ = 0xffffffff;
    std::string bank_name_;
    std::optional<Bank> bank_;
    std::span<const uint8_t> wave_data_;
    bool waves_loaded_ = false;
    std::optional<WaveArchive> wave_archive_;
    std::map<uint32_t, std::unique_ptr<Pcm>> waves_;
    std::vector<std::string> errors_;
};

} // namespace revosf2
