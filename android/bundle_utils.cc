// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/40284755): Remove this and spanify to fix the errors.
#pragma allow_unsafe_buffers
#endif

#include "base/android/bundle_utils.h"

#include <android/dlext.h>
#include <dlfcn.h>

#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "base/base_jni/BundleUtils_jni.h"
#include "base/check.h"
#include "base/files/file_path.h"
#include "base/notreached.h"

// These symbols are added by the lld linker when creating a partitioned shared
// library. The symbols live in the base library, and are used to properly load
// the other partitions (feature libraries) when needed.
struct PartitionIndexEntry {
  int32_t name_relptr;
  int32_t addr_relptr;
  uint32_t size;
};
static_assert(sizeof(PartitionIndexEntry) == 12U,
              "Unexpected PartitionIndexEntry size");

// Marked as weak_import because these symbols are lld-specific. The method that
// uses them will only be invoked in builds that have lld-generated partitions.
extern PartitionIndexEntry __part_index_begin[] __attribute__((weak_import));
extern PartitionIndexEntry __part_index_end[] __attribute__((weak_import));

namespace base {
namespace android {

namespace {

const void* ReadRelPtr(const int32_t* relptr) {
  return reinterpret_cast<const char*>(relptr) + *relptr;
}

}  // namespace

// static
std::string BundleUtils::ResolveLibraryPath(const std::string& library_name,
                                            const std::string& split_name) {
  JNIEnv* env = AttachCurrentThread();
  ScopedJavaLocalRef<jstring> java_path = Java_BundleUtils_getNativeLibraryPath(
      env, ConvertUTF8ToJavaString(env, library_name),
      ConvertUTF8ToJavaString(env, split_name));
  // TODO(crbug.com/40656179): Remove this tolerance.
  if (!java_path) {
    return std::string();
  }
  return ConvertJavaStringToUTF8(env, java_path);
}

// static
bool BundleUtils::IsBundle() {
  return Java_BundleUtils_isBundleForNative(AttachCurrentThread());
}

// static
void* BundleUtils::DlOpenModuleLibraryPartition(const std::string& library_name,
                                                const std::string& partition,
                                                const std::string& split_name) {
  // TODO(crbug.com/40656179): Remove this tolerance.
  std::string library_path = ResolveLibraryPath(library_name, split_name);
  if (library_path.empty()) {
    return nullptr;
  }

  // Linear search is required here because the partition descriptors are not
  // ordered. If a large number of partitions come into existence, lld could be
  // modified to sort the partitions.
  DCHECK(__part_index_begin != nullptr);
  DCHECK(__part_index_end != nullptr);
  for (const PartitionIndexEntry* part = __part_index_begin;
       part != __part_index_end; ++part) {
    std::string name(
        reinterpret_cast<const char*>(ReadRelPtr(&part->name_relptr)));
    if (name == partition) {
      android_dlextinfo info = {};
      info.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
      info.reserved_addr = const_cast<void*>(ReadRelPtr(&part->addr_relptr));
      info.reserved_size = part->size;

#if __ANDROID_API__ >= 24
      return android_dlopen_ext(library_path.c_str(), RTLD_LOCAL, &info);
#else
      // When targeting pre-N, such as for Cronet, android_dlopen_ext() might
      // not be available on the system.
      CHECK(0) << "android_dlopen_ext not available";
#endif
    }
  }

  NOTREACHED_IN_MIGRATION();
  return nullptr;
}

}  // namespace android
}  // namespace base
