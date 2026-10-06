#include "core/byte_writer.hpp"

#include "core/errors.hpp"

#include <cstring>
#include <string>

namespace mcs {

void ByteWriter::writeUnsigned(std::uint64_t v, std::size_t width)
{
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t shift = (endian_ == Endian::Little) ? i * 8 : (width - 1 - i) * 8;
        buffer_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFU));
    }
}

void ByteWriter::u8(std::uint8_t v)   { buffer_.push_back(v); }
void ByteWriter::u16(std::uint16_t v) { writeUnsigned(v, 2); }
void ByteWriter::u32(std::uint32_t v) { writeUnsigned(v, 4); }
void ByteWriter::u64(std::uint64_t v) { writeUnsigned(v, 8); }

// Two's-complement reinterpretation is well defined for unsigned conversion.
void ByteWriter::i8(std::int8_t v)   { u8(static_cast<std::uint8_t>(v)); }
void ByteWriter::i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
void ByteWriter::i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
void ByteWriter::i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

void ByteWriter::f32(float v)
{
    static_assert(sizeof(float) == 4, "IEEE-754 single precision required");
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    u32(bits);
}

void ByteWriter::f64(double v)
{
    static_assert(sizeof(double) == 8, "IEEE-754 double precision required");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    u64(bits);
}

void ByteWriter::fixedString(std::string_view text, std::size_t size)
{
    if (text.size() >= size) {
        throw EncodeError("string '" + std::string(text) + "' is too long: at most "
                          + std::to_string(size - 1) + " characters");
    }
    buffer_.insert(buffer_.end(), text.begin(), text.end());
    zeros(size - text.size());
}

void ByteWriter::bytes(const Bytes& data)
{
    buffer_.insert(buffer_.end(), data.begin(), data.end());
}

void ByteWriter::zeros(std::size_t count)
{
    buffer_.insert(buffer_.end(), count, 0);
}

void ByteWriter::patchU16BE(Bytes& buffer, std::size_t offset, std::uint16_t v)
{
    buffer.at(offset)     = static_cast<std::uint8_t>(v >> 8U);
    buffer.at(offset + 1) = static_cast<std::uint8_t>(v & 0xFFU);
}

} // namespace mcs
