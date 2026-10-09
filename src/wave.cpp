// SPDX-License-Identifier: MIT

// Wave decoding (see wave.h).

#include "wave.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "binary.h"

namespace revosf2
{

std::vector<int16_t> DecodeDspAdpcm(std::span<const uint8_t> data, uint32_t samples, std::span<const int16_t, 16> coefs,
                                    int16_t hist1, int16_t hist2)
{
    std::vector<int16_t> out;
    out.reserve(std::min<std::size_t>(samples, (data.size() + 7) / 8 * 14));
    int32_t h1 = hist1, h2 = hist2;
    for (std::size_t frame = 0; out.size() < samples; frame++)
    {
        const std::size_t base = frame * 8;
        if (base >= data.size())
        {
            break;
        }

        const uint8_t ps = data[base];
        const int predictor = (ps >> 4) & 7;
        const int32_t scale = 1 << (ps & 0x0f);
        const int32_t c1 = coefs[predictor * 2], c2 = coefs[predictor * 2 + 1];
        for (int i = 0; i < 14 && out.size() < samples; i++)
        {
            const std::size_t byte = base + 1 + i / 2;
            if (byte >= data.size())
            {
                break;
            }

            int nibble = (i & 1) ? (data[byte] & 0x0f) : (data[byte] >> 4); // high nibble first
            if (nibble >= 8)
            {
                nibble -= 16;
            }

            // Use 64 bits: the sum of two coefficient/sample products can overflow 32 bits.
            const int64_t v = (int64_t{nibble} * scale << 11) + 1024 + int64_t{c1} * h1 + int64_t{c2} * h2;
            const int16_t s = static_cast<int16_t>(std::clamp<int64_t>(v >> 11, -32768, 32767));
            h2 = h1;
            h1 = s;
            out.push_back(s);
        }
    }

    return out;
}

Pcm DecodeWave(const Wave& w)
{
    Pcm pcm;
    pcm.sample_rate = w.sample_rate;
    pcm.loop = w.loop;
    pcm.loop_start = w.loop_start;
    pcm.loop_end = w.loop_end;

    // NW4R SND plays only the first two channels (Bank::NoteOn, main.dol 0x80035b68). A corrupt file could claim
    // hundreds of full-length channels.
    const uint32_t n = w.loop_end;
    for (std::size_t c = 0; c < w.channels.size() && c < 2; c++)
    {
        const WaveChannel& ch = w.channels[c];
        std::vector<int16_t> s;
        switch (w.encoding)
        {
        case WaveEncoding::kPcm8:
            for (uint32_t i = 0; i < n && i < ch.data.size(); i++)
            {
                s.push_back(static_cast<int16_t>(static_cast<int8_t>(ch.data[i]) * 256));
            }
            break;

        case WaveEncoding::kPcm16:
            for (uint32_t i = 0; i < n && 2 * std::size_t{i} + 1 < ch.data.size(); i++)
            {
                s.push_back(static_cast<int16_t>(ch.data[2 * i] << 8 | ch.data[2 * i + 1]));
            }
            break;

        case WaveEncoding::kDspAdpcm:
            if (!ch.adpcm)
            {
                throw FormatError("DSP-ADPCM wave without coefficients");
            }

            s = DecodeDspAdpcm(ch.data, n, ch.adpcm->coefs, ch.adpcm->yn1, ch.adpcm->yn2);
            break;
        }

        // Decoders stop at the end of the available data. Reject a header claiming more samples; padding to that length
        // could allocate gigabytes.
        if (s.size() < n)
        {
            throw FormatError("wave data shorter than the declared sample count");
        }

        pcm.channels.push_back(std::move(s));
    }

    if (pcm.channels.empty())
    {
        throw FormatError("wave has no channels");
    }

    if (pcm.loop && pcm.loop_start >= pcm.loop_end)
    {
        pcm.loop = false;
    }

    return pcm;
}

namespace
{

// Unroll `pcm`'s loop so it starts at or after `min_start`, has at least `min_before` repeated samples before it, and
// is at least `min_length` samples long. Append `after` samples from the loop start for interpolation.
PreparedWave UnrollLoop(const Pcm& pcm, uint32_t min_start, uint32_t min_before, uint32_t min_length, uint32_t after)
{
    PreparedWave w;
    w.loop = pcm.loop;
    for (const auto& ch : pcm.channels)
    {
        std::vector<int16_t> src = ch;
        if (src.empty())
        {
            src.push_back(0); // a sample requires at least one point
        }

        if (!pcm.loop)
        {
            w.channels.push_back(std::move(src));
            continue;
        }

        const uint32_t ls = pcm.loop_start, le = pcm.loop_end, len = le - ls;
        const std::vector<int16_t> body(src.begin() + ls, src.begin() + le);
        uint32_t before = 0;
        while (ls + before * len < min_start || before * len < min_before)
        {
            before++;
        }

        uint32_t reps = 1;
        while (reps * len < min_length)
        {
            reps++;
        }

        std::vector<int16_t> out(src.begin(), src.begin() + ls);
        for (uint32_t i = 0; i < before; i++)
        {
            out.insert(out.end(), body.begin(), body.end());
        }

        w.loop_start = static_cast<uint32_t>(out.size());
        for (uint32_t i = 0; i < reps; i++)
        {
            out.insert(out.end(), body.begin(), body.end());
        }
        w.loop_end = static_cast<uint32_t>(out.size());

        for (uint32_t i = 0; i < after; i++)
        {
            out.push_back(body[i % len]);
        }

        w.channels.push_back(std::move(out));
    }

    return w;
}

} // namespace

PreparedWave PrepareForSoundFont(const Pcm& pcm)
{
    return UnrollLoop(pcm, 8, 0, 32, 32);
}

PreparedWave PrepareForSfz(const Pcm& pcm)
{
    const uint32_t crossfade = (pcm.sample_rate * 4 + 999) / 1000; // 4 ms
    return UnrollLoop(pcm, 0, std::max<uint32_t>(crossfade, 64), 64, 64);
}

} // namespace revosf2
