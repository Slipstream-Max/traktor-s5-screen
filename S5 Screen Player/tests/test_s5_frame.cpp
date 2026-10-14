#include "../src/s5_frame.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr size_t kPixels = 480 * 272;

void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

// Independent reader for captured S5 frames. 0x02 advances over existing
// framebuffer pixels; the player emits self-contained 0x00/0x01 frames only.
s5::Pixels decode(const Bytes& bytes, bool allow_skip = false) {
    require(bytes.size() >= 24, "short frame");
    Bytes header = {0x84, 0, 0, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xe0, 1, 0x10};
    require(bytes[2] <= 1, "invalid screen");
    header[2] = bytes[2];
    require(std::equal(header.begin(), header.end(), bytes.begin()), "bad header");
    const Bytes footer = {3, 0, 0, 0, 0x40, 0, bytes[2], 0};
    require(std::equal(footer.begin(), footer.end(), bytes.end() - 8), "bad footer");
    s5::Pixels pixels(kPixels, 0xa55a);
    size_t input = 16;
    size_t output = 0;
    const size_t end = bytes.size() - 8;
    auto word = [&]() -> std::uint16_t {
        require(input + 2 <= end, "truncated data");
        auto value = static_cast<std::uint16_t>((bytes[input] << 8) | bytes[input + 1]);
        input += 2;
        return value;
    };
    while (input < end) {
        require(input + 4 <= end, "truncated command");
        const unsigned command = bytes[input++];
        require(bytes[input++] == 0, "unexpected flags");
        const size_t count = static_cast<size_t>(word()) * 2;
        require(count && count <= kPixels - output, "pixel count overflow");
        if (command == 0) {
            for (size_t i = 0; i < count; ++i) pixels[output + i] = word();
        } else if (command == 1) {
            const std::uint16_t first = word(), second = word();
            for (size_t i = 0; i < count; i += 2) {
                pixels[output + i] = first;
                pixels[output + i + 1] = second;
            }
        } else {
            require(command == 2 && allow_skip, "unexpected command or delta frame");
        }
        output += count;
    }
    require(output == kPixels && input == end, "frame does not cover exactly 480x272");
    return pixels;
}

void round_trip(const s5::Pixels& pixels) {
    for (std::uint8_t screen : std::array<std::uint8_t, 2>{0, 1}) {
        const auto encoded = s5::encode_frame(screen, pixels);
        require(decode(encoded) == pixels, "pixel mismatch after round-trip");
        require(encoded.size() % 4 == 0, "payload is not pair aligned");
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        // Matches the 32-byte red pattern confirmed on the real left S5 screen.
        const Bytes red = {0x84,0,0,0x60,0,0,0,0,0,0,0,0,1,0xe0,1,0x10,
                           1,0,0xff,0,0xf8,0,0xf8,0,3,0,0,0,0x40,0,0,0};
        require(s5::encode_frame(0, s5::Pixels(kPixels, 0xf800)) == red,
                "known solid-color packet changed");
        for (std::uint16_t color : std::array<std::uint16_t, 5>{0, 0xffff, 0xf800, 0x07e0, 0x001f}) {
            round_trip(s5::Pixels(kPixels, color));
        }
        s5::Pixels alternating(kPixels);
        for (size_t i = 0; i < kPixels; ++i) alternating[i] = i % 2 ? 0x07e0 : 0xf800;
        round_trip(alternating);
        require(s5::encode_frame(1, alternating).size() == 32,
                "unequal repeating pair was not compressed");
        s5::Pixels literal(kPixels);
        for (size_t i = 0; i < kPixels; ++i) literal[i] = static_cast<std::uint16_t>(i);
        const auto full_literal = s5::encode_frame(0, literal);
        require(full_literal.size() == 261148 && full_literal[16] == 0 &&
                full_literal[18] == 0xff && full_literal[19] == 0,
                "literal count must be 65280 pairs, not 130560 pixels");
        round_trip(literal);
        std::mt19937 random(0x1420);
        for (int trial = 0; trial < 32; ++trial) {
            s5::Pixels mixed(kPixels);
            for (size_t i = 0; i < kPixels;) {
                const size_t count = std::min<size_t>(1 + random() % 971, kPixels - i);
                const auto color = static_cast<std::uint16_t>(random());
                for (size_t j = 0; j < count; ++j) {
                    mixed[i++] = trial % 2 ? color : static_cast<std::uint16_t>(random());
                }
            }
            round_trip(mixed);
        }
        bool rejected = false;
        try { s5::encode_frame(2, literal); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid screen was accepted");
        rejected = false;
        try { s5::encode_frame(0, s5::Pixels(1)); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "invalid pixel count was accepted");
        for (int i = 1; i < argc; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            require(file.is_open(), "cannot open captured frame");
            Bytes bytes{std::istreambuf_iterator<char>(file), {}};
            round_trip(decode(bytes, true));
        }
        std::cout << "S5 encoder: golden packet, literal counts, pair alignment, mixed frames, "
                     "and " << argc - 1 << " captured frames passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
