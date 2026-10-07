#pragma once

#include "pipkin/model.h"

namespace pipkin {

struct LineReader {
    char bytes[kMaxPacketBytes + 1]{};
    std::size_t size = 0;
    bool overflow = false;

    template <typename Consume> void push(char byte, Consume consume) {
        if (byte == '\n') {
            if (!overflow && size != 0) {
                const auto length = bytes[size - 1] == '\r' ? size - 1 : size;
                if (length != 0 && length <= kMaxPacketBytes)
                    consume(std::string_view(bytes, length));
            }
            size = 0;
            overflow = false;
        } else if (size == sizeof(bytes)) {
            overflow = true;
        } else if (!overflow) {
            bytes[size++] = byte;
        }
    }
};

} // namespace pipkin
