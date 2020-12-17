// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/partition_allocator/partition_oom.h"

#include "base/allocator/partition_allocator/oom.h"
#include "build/build_config.h"

namespace base {
namespace internal {

OomFunction g_oom_handling_function = nullptr;

void NOINLINE PartitionExcessiveAllocationSize(size_t size) {
  OOM_CRASH(size);
}

#if !defined(ARCH_CPU_64_BITS)
NOINLINE void PartitionOutOfMemoryWithLotsOfUncommitedPages(size_t size) {
  OOM_CRASH(size);
}

[[noreturn]] NOINLINE void PartitionOutOfMemoryWithLargeVirtualSize(
    size_t virtual_size) {
  OOM_CRASH(virtual_size);
}

#endif  // !defined(ARCH_CPU_64_BITS)

}  // namespace internal
}  // namespace base
