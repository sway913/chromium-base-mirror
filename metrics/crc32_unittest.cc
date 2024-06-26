// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/40284755): Remove this and spanify to fix the errors.
#pragma allow_unsafe_buffers
#endif

#include "base/metrics/crc32.h"

#include <stdint.h>

#include "base/containers/span.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base {

// Table was generated similarly to sample code for CRC-32 given on:
// http://www.w3.org/TR/PNG/#D-CRCAppendix.
TEST(Crc32Test, TableTest) {
  for (int i = 0; i < 256; ++i) {
    uint32_t checksum = i;
    for (int j = 0; j < 8; ++j) {
      const uint32_t kReversedPolynomial = 0xEDB88320L;
      if (checksum & 1)
        checksum = kReversedPolynomial ^ (checksum >> 1);
      else
        checksum >>= 1;
    }
    EXPECT_EQ(kCrcTable[i], checksum);
  }
}

// A CRC of nothing should always be zero.
TEST(Crc32Test, ZeroTest) {
  span<const uint8_t> empty_data;
  EXPECT_EQ(0U, Crc32(0, empty_data));
}

}  // namespace base
