#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

bool DecodeJpegToRgba(const uint8_t* data,
                      size_t size,
                      std::vector<uint8_t>* rgba,
                      int* width,
                      int* height,
                      std::string* error);
