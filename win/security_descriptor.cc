// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/win/security_descriptor.h"

#include <aclapi.h>
#include <sddl.h>
#include <windows.h>

#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/numerics/checked_math.h"
#include "base/win/scoped_localalloc.h"

namespace base::win {

namespace {
template <typename T>
absl::optional<T> CloneValue(const absl::optional<T>& value) {
  if (!value)
    return absl::nullopt;
  return value->Clone();
}

PSID UnwrapSid(const absl::optional<Sid>& sid) {
  if (!sid)
    return nullptr;
  return sid->GetPSID();
}

PACL UnwrapAcl(const absl::optional<AccessControlList>& acl) {
  if (!acl)
    return nullptr;
  return acl->get();
}

SE_OBJECT_TYPE ConvertObjectType(SecurityObjectType object_type) {
  switch (object_type) {
    case SecurityObjectType::kFile:
      return SE_FILE_OBJECT;
    case SecurityObjectType::kRegistry:
      return SE_REGISTRY_KEY;
    case SecurityObjectType::kWindow:
      return SE_WINDOW_OBJECT;
    case SecurityObjectType::kKernel:
      return SE_KERNEL_OBJECT;
  }
  return SE_UNKNOWN_OBJECT_TYPE;
}

template <typename T>
absl::optional<SecurityDescriptor> GetSecurityDescriptor(
    T object,
    SecurityObjectType object_type,
    SECURITY_INFORMATION security_info,
    DWORD(WINAPI* get_sd)(T,
                          SE_OBJECT_TYPE,
                          SECURITY_INFORMATION,
                          PSID*,
                          PSID*,
                          PACL*,
                          PACL*,
                          PSECURITY_DESCRIPTOR*)) {
  PSECURITY_DESCRIPTOR sd = nullptr;

  DWORD error = get_sd(object, ConvertObjectType(object_type), security_info,
                       nullptr, nullptr, nullptr, nullptr, &sd);
  if (error != ERROR_SUCCESS) {
    ::SetLastError(error);
    DPLOG(ERROR) << "Failed getting security descriptor for object.";
    return absl::nullopt;
  }
  auto sd_ptr = TakeLocalAlloc(sd);
  return SecurityDescriptor::FromPointer(sd_ptr.get());
}

template <typename T>
bool SetSecurityDescriptor(const SecurityDescriptor& sd,
                           T object,
                           SecurityObjectType object_type,
                           SECURITY_INFORMATION security_info,
                           DWORD(WINAPI* set_sd)(T,
                                                 SE_OBJECT_TYPE,
                                                 SECURITY_INFORMATION,
                                                 PSID,
                                                 PSID,
                                                 PACL,
                                                 PACL)) {
  security_info &= ~(PROTECTED_DACL_SECURITY_INFORMATION |
                     UNPROTECTED_DACL_SECURITY_INFORMATION |
                     PROTECTED_SACL_SECURITY_INFORMATION |
                     UNPROTECTED_SACL_SECURITY_INFORMATION);
  if (security_info & DACL_SECURITY_INFORMATION) {
    if (sd.dacl_protected()) {
      security_info |= PROTECTED_DACL_SECURITY_INFORMATION;
    } else {
      security_info |= UNPROTECTED_DACL_SECURITY_INFORMATION;
    }
  }
  if (security_info & SACL_SECURITY_INFORMATION) {
    if (sd.sacl_protected()) {
      security_info |= PROTECTED_SACL_SECURITY_INFORMATION;
    } else {
      security_info |= UNPROTECTED_SACL_SECURITY_INFORMATION;
    }
  }
  DWORD error = set_sd(object, ConvertObjectType(object_type), security_info,
                       UnwrapSid(sd.owner()), UnwrapSid(sd.group()),
                       UnwrapAcl(sd.dacl()), UnwrapAcl(sd.sacl()));
  if (error != ERROR_SUCCESS) {
    ::SetLastError(error);
    DPLOG(ERROR) << "Failed setting DACL for object.";
    return false;
  }
  return true;
}

absl::optional<Sid> GetSecurityDescriptorSid(
    PSECURITY_DESCRIPTOR sd,
    BOOL(WINAPI* get_sid)(PSECURITY_DESCRIPTOR, PSID*, LPBOOL)) {
  PSID sid;
  BOOL defaulted;
  if (!get_sid(sd, &sid, &defaulted) || !sid) {
    return absl::nullopt;
  }
  return Sid::FromPSID(sid);
}

absl::optional<AccessControlList> GetSecurityDescriptorAcl(
    PSECURITY_DESCRIPTOR sd,
    BOOL(WINAPI* get_acl)(PSECURITY_DESCRIPTOR, LPBOOL, PACL*, LPBOOL)) {
  PACL acl;
  BOOL present;
  BOOL defaulted;
  if (!get_acl(sd, &present, &acl, &defaulted) || !present) {
    return absl::nullopt;
  }
  return AccessControlList::FromPACL(acl);
}

}  // namespace

SecurityDescriptor::SelfRelative::SelfRelative(const SelfRelative&) = default;
SecurityDescriptor::SelfRelative::~SelfRelative() = default;
SecurityDescriptor::SelfRelative::SelfRelative(std::vector<uint8_t>&& sd)
    : sd_(sd) {}

absl::optional<SecurityDescriptor> SecurityDescriptor::FromPointer(
    PSECURITY_DESCRIPTOR sd) {
  if (!sd || !::IsValidSecurityDescriptor(sd)) {
    ::SetLastError(ERROR_INVALID_SECURITY_DESCR);
    return absl::nullopt;
  }

  SECURITY_DESCRIPTOR_CONTROL control;
  DWORD revision;
  if (!::GetSecurityDescriptorControl(sd, &control, &revision)) {
    return absl::nullopt;
  }

  return SecurityDescriptor{
      GetSecurityDescriptorSid(sd, ::GetSecurityDescriptorOwner),
      GetSecurityDescriptorSid(sd, ::GetSecurityDescriptorGroup),
      GetSecurityDescriptorAcl(sd, ::GetSecurityDescriptorDacl),
      !!(control & SE_DACL_PROTECTED),
      GetSecurityDescriptorAcl(sd, ::GetSecurityDescriptorSacl),
      !!(control & SE_SACL_PROTECTED)};
}

absl::optional<SecurityDescriptor> SecurityDescriptor::FromFile(
    const base::FilePath& path,
    SECURITY_INFORMATION security_info) {
  return FromName(path.value().c_str(), SecurityObjectType::kFile,
                  security_info);
}

absl::optional<SecurityDescriptor> SecurityDescriptor::FromName(
    const std::wstring& name,
    SecurityObjectType object_type,
    SECURITY_INFORMATION security_info) {
  return GetSecurityDescriptor(name.c_str(), object_type, security_info,
                               ::GetNamedSecurityInfo);
}

absl::optional<SecurityDescriptor> SecurityDescriptor::FromHandle(
    HANDLE handle,
    SecurityObjectType object_type,
    SECURITY_INFORMATION security_info) {
  return GetSecurityDescriptor<HANDLE>(handle, object_type, security_info,
                                       ::GetSecurityInfo);
}

absl::optional<SecurityDescriptor> SecurityDescriptor::FromSddl(
    const std::wstring& sddl) {
  PSECURITY_DESCRIPTOR sd;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptor(
          sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
    return absl::nullopt;
  }
  auto sd_ptr = TakeLocalAlloc(sd);
  return FromPointer(sd_ptr.get());
}

SecurityDescriptor::SecurityDescriptor() = default;
SecurityDescriptor::SecurityDescriptor(SecurityDescriptor&&) = default;
SecurityDescriptor& SecurityDescriptor::operator=(SecurityDescriptor&&) =
    default;
SecurityDescriptor::~SecurityDescriptor() = default;

bool SecurityDescriptor::WriteToFile(const base::FilePath& path,
                                     SECURITY_INFORMATION security_info) const {
  return WriteToName(path.value().c_str(), SecurityObjectType::kFile,
                     security_info);
}

bool SecurityDescriptor::WriteToName(const std::wstring& name,
                                     SecurityObjectType object_type,
                                     SECURITY_INFORMATION security_info) const {
  return SetSecurityDescriptor<wchar_t*>(
      *this, const_cast<wchar_t*>(name.c_str()), object_type, security_info,
      ::SetNamedSecurityInfo);
}

bool SecurityDescriptor::WriteToHandle(
    HANDLE handle,
    SecurityObjectType object_type,
    SECURITY_INFORMATION security_info) const {
  return SetSecurityDescriptor<HANDLE>(*this, handle, object_type,
                                       security_info, ::SetSecurityInfo);
}

absl::optional<std::wstring> SecurityDescriptor::ToSddl(
    SECURITY_INFORMATION security_info) const {
  SECURITY_DESCRIPTOR sd = {};
  ToAbsolute(sd);
  LPWSTR sddl;
  if (!::ConvertSecurityDescriptorToStringSecurityDescriptor(
          &sd, SDDL_REVISION_1, security_info, &sddl, nullptr)) {
    return absl::nullopt;
  }
  auto sddl_ptr = TakeLocalAlloc(sddl);
  return sddl_ptr.get();
}

void SecurityDescriptor::ToAbsolute(SECURITY_DESCRIPTOR& sd) const {
  memset(&sd, 0, sizeof(sd));
  sd.Revision = SECURITY_DESCRIPTOR_REVISION;
  sd.Owner = owner_ ? owner_->GetPSID() : nullptr;
  sd.Group = group_ ? group_->GetPSID() : nullptr;
  if (dacl_) {
    sd.Dacl = dacl_->get();
    sd.Control |= SE_DACL_PRESENT;
    if (dacl_protected_) {
      sd.Control |= SE_DACL_PROTECTED;
    }
  }
  if (sacl_) {
    sd.Sacl = sacl_->get();
    sd.Control |= SE_SACL_PRESENT;
    if (sacl_protected_) {
      sd.Control |= SE_SACL_PROTECTED;
    }
  }
  DCHECK(::IsValidSecurityDescriptor(&sd));
}

absl::optional<SecurityDescriptor::SelfRelative>
SecurityDescriptor::ToSelfRelative() const {
  SECURITY_DESCRIPTOR sd = {};
  ToAbsolute(sd);
  DWORD size = sizeof(SECURITY_DESCRIPTOR_MIN_LENGTH);
  std::vector<uint8_t> buffer(SECURITY_DESCRIPTOR_MIN_LENGTH);
  if (::MakeSelfRelativeSD(&sd, buffer.data(), &size)) {
    return SelfRelative(std::move(buffer));
  }

  if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return absl::nullopt;
  }

  buffer.resize(size);
  if (!::MakeSelfRelativeSD(&sd, buffer.data(), &size)) {
    return absl::nullopt;
  }
  return SelfRelative(std::move(buffer));
}

SecurityDescriptor SecurityDescriptor::Clone() const {
  return SecurityDescriptor{CloneValue(owner_), CloneValue(group_),
                            CloneValue(dacl_),  dacl_protected_,
                            CloneValue(sacl_),  sacl_protected_};
}

bool SecurityDescriptor::SetMandatoryLabel(DWORD integrity_level,
                                           DWORD inheritance,
                                           DWORD mandatory_policy) {
  absl::optional<AccessControlList> sacl =
      AccessControlList::FromMandatoryLabel(integrity_level, inheritance,
                                            mandatory_policy);
  if (!sacl) {
    return false;
  }
  sacl_ = std::move(*sacl);
  return true;
}

bool SecurityDescriptor::SetDaclEntries(
    const std::vector<ExplicitAccessEntry>& entries) {
  if (!dacl_) {
    dacl_ = AccessControlList{};
  }
  return dacl_->SetEntries(entries);
}

bool SecurityDescriptor::SetDaclEntry(const Sid& sid,
                                      SecurityAccessMode mode,
                                      DWORD access_mask,
                                      DWORD inheritance) {
  if (!dacl_) {
    dacl_ = AccessControlList{};
  }
  return dacl_->SetEntry(sid, mode, access_mask, inheritance);
}

SecurityDescriptor::SecurityDescriptor(absl::optional<Sid>&& owner,
                                       absl::optional<Sid>&& group,
                                       absl::optional<AccessControlList>&& dacl,
                                       bool dacl_protected,
                                       absl::optional<AccessControlList>&& sacl,
                                       bool sacl_protected) {
  owner_.swap(owner);
  group_.swap(group);
  dacl_.swap(dacl);
  dacl_protected_ = dacl_protected;
  sacl_.swap(sacl);
  sacl_protected_ = sacl_protected;
}

}  // namespace base::win
