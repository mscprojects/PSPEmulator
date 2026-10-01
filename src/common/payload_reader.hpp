#pragma once

#include "common/payload.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace psp
{

// Read a borrowed payload sequentially. Successful reads advance the byte position;
// failed reads and seeks throw std::invalid_argument without changing it.
class PayloadReader
{
public:
    // Select the byte order used by all multi-byte reads.
    PayloadReader(PayloadSpan payload, std::endian endianness);

    std::size_t position() const;
    std::size_t remaining() const;

    // Set an absolute byte position. Seeking exactly to the end is valid.
    void seek(std::size_t offset);
    // Advance by a byte count, for padding or fields that are not needed.
    void skip(std::size_t size);
    // Validate an absolute range without changing the position.
    void require_range(std::size_t offset, std::size_t size) const;

    // Borrow the next bytes without copying. An empty read at EOF is valid.
    [[nodiscard]] PayloadSpan read_bytes(std::size_t size);
    [[nodiscard]] std::uint8_t read_u8();
    // Multi-byte reads support unaligned positions and either byte order.
    [[nodiscard]] std::uint16_t read_u16();
    [[nodiscard]] std::uint32_t read_u32();

private:
    PayloadSpan payload_;
    std::endian endianness_;
    std::size_t position_{};
};

} // namespace psp
