#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace s5 {

constexpr unsigned kWidth = 480;
constexpr unsigned kHeight = 272;
using Pixels = std::vector<std::uint16_t>;

inline void append_be16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

inline void append_command(std::vector<std::uint8_t>& output, std::uint8_t command,
                    std::uint8_t flags, std::size_t count) {
    if (count == 0 || count > 0xffff) {
        throw std::runtime_error("frame command count is outside 16-bit range");
    }
    output.push_back(command);
    output.push_back(flags);
    output.push_back(static_cast<std::uint8_t>(count >> 8));
    output.push_back(static_cast<std::uint8_t>(count & 0xff));
}

// Encode the raw payload accepted by 0x80892880. The 16-byte header, RGB565
// byte order, command records, and eight-byte trailer follow captured S5
// payloads. A repeat record describes a two-pixel RGB565 pair (four bytes),
// which is why a solid 480x272 frame is 65280 repeat units and only 32 bytes.
// Non-repeating pixels use literal records with one two-pixel pair per count.
inline std::vector<std::uint8_t> encode_frame(std::uint8_t screen,
                                       const Pixels& pixels) {
    if (screen > 1) {
        throw std::runtime_error("screen must be 0 or 1");
    }
    if (pixels.size() != static_cast<size_t>(kWidth) * kHeight) {
        throw std::runtime_error("frame must contain exactly 480x272 pixels");
    }

    std::vector<std::uint8_t> output;
    output.reserve(300000);
    const std::uint8_t header[] = {
        0x84, 0x00, screen, 0x60, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0xe0, 0x01, 0x10,
    };
    output.insert(output.end(), std::begin(header), std::end(header));

    const size_t pair_total = pixels.size() / 2;
    size_t pair = 0;
    while (pair < pair_total) {
        const std::uint16_t first = pixels[pair * 2];
        const std::uint16_t second = pixels[pair * 2 + 1];
        size_t repeated = 1;
        while (pair + repeated < pair_total && repeated < 0xffff &&
               pixels[(pair + repeated) * 2] == first &&
               pixels[(pair + repeated) * 2 + 1] == second) {
            ++repeated;
        }
        if (repeated >= 2) {
            append_command(output, 0x01, 0x00, repeated);
            append_be16(output, first);
            append_be16(output, second);
            pair += repeated;
            continue;
        }

        const size_t literal_start = pair;
        ++pair;
        while (pair < pair_total && pair - literal_start < 0xffff) {
            const std::uint16_t literal_first = pixels[pair * 2];
            const std::uint16_t literal_second = pixels[pair * 2 + 1];
            size_t next_repeated = 1;
            while (pair + next_repeated < pair_total && next_repeated < 0xffff &&
                   pixels[(pair + next_repeated) * 2] == literal_first &&
                   pixels[(pair + next_repeated) * 2 + 1] == literal_second) {
                ++next_repeated;
            }
            if (next_repeated >= 2) {
                break;
            }
            ++pair;
        }
        const size_t literal_pairs = pair - literal_start;
        append_command(output, 0x00, 0x00, literal_pairs);
        for (size_t index = literal_start * 2; index < pair * 2; ++index) {
            append_be16(output, pixels[index]);
        }
    }

    const std::uint8_t trailer[] = {0x03, 0x00, 0x00, 0x00,
                                    0x40, 0x00, screen, 0x00};
    output.insert(output.end(), std::begin(trailer), std::end(trailer));
    return output;
}

} // namespace s5
