#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace simul {

using Byte = std::uint8_t;
using Buffer = std::vector<Byte>;

constexpr Byte STX = 0x02;
constexpr Byte ETX = 0x03;
constexpr Byte DLE = 0x80;

constexpr Byte HEADER_HI = 0xC0;
constexpr Byte HEADER_LO = 0xC1;
constexpr Byte TRAILER_HI = 0xFF;
constexpr Byte TRAILER_LO = 0xFF;

constexpr std::size_t DATA_SIZE = 32;

// Logical payload between STX and ETX, before DLE escaping.
//   C0 C1 | DATA (32) | CRC16 (2) | FF FF
constexpr std::size_t PAYLOAD_SIZE = 2 + DATA_SIZE + 2 + 2;

using Data = std::array<Byte, DATA_SIZE>;

// CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final xor.
std::uint16_t crc16(const Byte* data, std::size_t size);

// Fill 32 random bytes in the full 0x00..0xFF range using a secure-ish source.
void randomData(Data& out);

// Build a complete on-wire frame from the 32-byte data payload.
//
// Returned frame layout:
//   STX | escape(C0 C1 | DATA | CRC16 | FF FF) | ETX
//
// DLE escape rules:
//   0x02 -> 0x80 0x82
//   0x03 -> 0x80 0x83
//   0x80 -> 0x80 0x80
Buffer buildFrame(const Data& data);

// Try to extract a single complete frame from the receive buffer.
//
// Return values:
//   NeedMore  - no full frame yet, keep reading.
//   Ok        - one frame was successfully decoded and verified; data filled.
//   Invalid   - a frame boundary was seen but the content failed validation
//               (bad escape, length, header, trailer, or CRC).
// In both Ok and Invalid cases, buffer bytes up to and including the ETX
// that closed the attempt are consumed.
enum class Parse { NeedMore, Ok, Invalid };
Parse parseFrame(Buffer& inOut, Data& dataOut);

}  // namespace simul
