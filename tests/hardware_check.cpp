#include "board.h"
#include "line_reader.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    pipkin::LineReader reader;
    std::vector<std::string> lines;
    auto receive = [&](std::string_view line) { lines.emplace_back(line); };
    auto feed = [&](std::string_view bytes) {
        for (char byte : bytes)
            reader.push(byte, receive);
    };
    feed("first\r\nsec");
    assert((lines == std::vector<std::string>{"first"}));
    feed("ond\n\n");
    assert((lines == std::vector<std::string>{"first", "second"}));
    feed(std::string(pipkin::kMaxPacketBytes, 'a') + "\n");
    assert(lines.back().size() == pipkin::kMaxPacketBytes);
    feed(std::string(pipkin::kMaxPacketBytes, 'b') + "\r\n");
    assert(lines.back() == std::string(pipkin::kMaxPacketBytes, 'b'));
    const auto accepted = lines.size();
    feed(std::string(pipkin::kMaxPacketBytes + 1, 'a') + "\n");
    assert(lines.size() == accepted);
    feed(std::string(pipkin::kMaxPacketBytes + 1, 'a') + "discard\n");
    assert(lines.size() == accepted);
    feed("recovered\n");
    assert(lines.back() == "recovered");
    reader.overflow = true;
    feed("discard after UART error\nvalid\n");
    assert(lines.back() == "valid" && lines.size() == accepted + 2);
    assert(board::touch_coordinate(200, 200, 3800, 320) == 0);
    assert(board::touch_coordinate(3800, 200, 3800, 320) == 319);
    assert(board::touch_coordinate(4095, 200, 3800, 320) == 319);
    assert(board::touch_coordinate(0, 200, 3800, 320) == 0);
    assert(board::touch_coordinate(200, 3800, 200, 240) == 239);
    assert(board::touch_coordinate(3800, 3800, 200, 240) == 0);
}
