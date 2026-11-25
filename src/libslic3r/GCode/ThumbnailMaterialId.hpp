#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace Slic3r {

// In legacy no-light thumbnails, slot 256 has alpha zero, just like the background.
constexpr bool thumbnail_material_id_needs_rgb(std::size_t material_id) { return material_id >= 256; }

// Zero is the transparent background. Physical and virtual material IDs are one-based.
constexpr std::array<uint8_t, 3> encode_thumbnail_material_id(unsigned int id)
{
    return {uint8_t(id), uint8_t(id >> 8), uint8_t(id >> 16)};
}

constexpr unsigned int decode_thumbnail_material_id(const uint8_t* rgb)
{
    return unsigned(rgb[0]) | (unsigned(rgb[1]) << 8) | (unsigned(rgb[2]) << 16);
}

} // namespace Slic3r
