/**
 * @file src/nvenc/nvenc_encoded_frame.h
 * @brief Declarations for NVENC encoded frame.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

namespace nvenc {

  /**
   * @brief Encoded frame.
   */
  struct nvenc_encoded_frame {
    std::vector<uint8_t> data;
    uint64_t frame_index = 0;
    bool idr = false;
    bool after_ref_frame_invalidation = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string codec = "unknown";
  };

}  // namespace nvenc
