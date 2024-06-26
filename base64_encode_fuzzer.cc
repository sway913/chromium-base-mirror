// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <string_view>

#include "base/base64.h"
#include "base/check_op.h"

// Encode some random data, and then decode it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  base::span<const uint8_t> data_span(data, size);
  std::string_view data_piece(reinterpret_cast<const char*>(data), size);

  const std::string encode_output = base::Base64Encode(data_span);
  std::string decode_output;
  CHECK(base::Base64Decode(encode_output, &decode_output));
  CHECK_EQ(data_piece, decode_output);

  // Also run the std::string_view variant and check that it gives the same
  // results.
  CHECK_EQ(encode_output, base::Base64Encode(data_piece));

  return 0;
}
