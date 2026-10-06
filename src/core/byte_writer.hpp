#pragma once

#include "core/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

namespace mcs {

// Appends values to a growing byte buffer in a fixed byte order.
// Used by the payload encoder (target byte order) and by packet formats.
class ByteWriter {
public:
    explicit ByteWriter(Endian endian) : endian_(endian) {}

    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);

    void i8(std::int8_t v);
    void i16(std::int16_t v);
    void i32(std::int32_t v);
    void i64(std::int64_t v);

    void f32(float v);
    void f64(double v);

    // Writes `size` bytes: the text followed by NUL fill.
    // Throws EncodeError if text.size() >= size (a terminating NUL must fit).
    void fixedString(std::string_view text, std::size_t size);

    void bytes(const Bytes& data);
    void zeros(std::size_t count);

    std::size_t size() const { return buffer_.size(); }
    const Bytes& data() const { return buffer_; }
    Bytes take() { return std::move(buffer_); }

    // Overwrite 2 bytes at `offset` with a big-endian value (header patching).
    static void patchU16BE(Bytes& buffer, std::size_t offset, std::uint16_t v);

private:
    void writeUnsigned(std::uint64_t v, std::size_t width);

    Endian endian_;
    Bytes buffer_;
};

} // namespace mcs
