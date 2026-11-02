#include "ProtocolLimits.h"
#include <cassert>
#include <climits>
#include <cstring>
#include <string>

using pixelpad::JsonLine;

int main() {
    using namespace pixelpad;
    assert(validStandby(1, 1, kFrameBytes));
    assert(validStandby(256, 60, 256 * kFrameBytes));
    assert(!validStandby(0, 8, 0));
    assert(!validStandby(-1, 8, SIZE_MAX));
    assert(!validStandby(INT_MAX, 8, SIZE_MAX));
    assert(!validStandby(257, 8, 257 * kFrameBytes));
    assert(!validStandby(1, 0, kFrameBytes));
    assert(!validStandby(1, 61, kFrameBytes));
    assert(!validStandby(2, 8, kFrameBytes));
    assert(!validStandby(1, 8, kFrameBytes + 1));

    JsonLine parser;
    char output[kJsonBytes];
    const std::string header = "{\"cmd\":\"recv\",\"size\":8112}\r\n";
    // All fragment boundaries, including a header and binary data in one ATT write.
    for (size_t split = 0; split <= header.size(); ++split) {
        parser.reset();
        size_t lines = 0;
        for (const auto& fragment : {header.substr(0, split), header.substr(split)}) {
            for (uint8_t ch : fragment) {
                if (parser.push(ch, output, sizeof(output)) == JsonLine::Result::Complete) ++lines;
            }
        }
        assert(lines == 1);
        assert(!strcmp(output, "{\"cmd\":\"recv\",\"size\":8112}"));
    }
    std::string packet = header + std::string("\0\n\r\xff", 4);
    size_t consumed = 0;
    while (consumed < packet.size()) {
        if (parser.push(packet[consumed++], output, sizeof(output)) == JsonLine::Result::Complete) break;
    }
    assert(consumed == header.size());
    assert(packet.size() - consumed == 4); // All binary bytes remain for the payload reader.

    // Exact limit succeeds; an oversized line cannot execute a valid-looking suffix.
    for (size_t length : {kJsonBytes - 1, kJsonBytes, kJsonBytes * 4}) {
        parser.reset();
        const std::string line(length, 'x');
        for (uint8_t ch : line) assert(parser.push(ch, output, sizeof(output)) == JsonLine::Result::Incomplete);
        const auto result = parser.push('\n', output, sizeof(output));
        assert(result == (length < kJsonBytes ? JsonLine::Result::Complete : JsonLine::Result::TooLong));
        for (uint8_t ch : std::string("{}")) parser.push(ch, output, sizeof(output));
        assert(parser.push('\n', output, sizeof(output)) == JsonLine::Result::Complete);
        assert(!strcmp(output, "{}"));
    }
    for (uint8_t ch : std::string(kJsonBytes + 20, 'x') + "{\"cmd\":\"mode\"}")
        assert(parser.push(ch, output, sizeof(output)) == JsonLine::Result::Incomplete);
    assert(parser.push('\n', output, sizeof(output)) == JsonLine::Result::TooLong);
    // A disconnect resets a partial header instead of combining clients' JSON.
    for (uint8_t ch : std::string("{\"cmd\":")) parser.push(ch, output, sizeof(output));
    parser.reset();
    parser.push('{', output, sizeof(output)); parser.push('}', output, sizeof(output));
    assert(parser.push('\n', output, sizeof(output)) == JsonLine::Result::Complete);
    assert(!strcmp(output, "{}"));
    char small[2] = {'?', '!'};
    parser.push('a', small, sizeof(small)); parser.push('b', small, sizeof(small));
    assert(parser.push('\n', small, sizeof(small)) == JsonLine::Result::TooLong);
    assert(small[0] == '?' && small[1] == '!'); // No silent truncation or buffer write.
}
