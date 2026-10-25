// PNG, JPEG and the rest through WIC, which ships with Windows.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace image_decoder {

constexpr uint32_t kMaxSide = 4096;

// The file's bytes in, premultiplied RGBA out.
bool decode(const std::string& bytes, uint32_t& width, uint32_t& height,
            std::vector<uint8_t>& rgba);

}  // namespace image_decoder
