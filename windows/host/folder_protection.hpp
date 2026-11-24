// The protections a folder of private captures gets (pet-camera.md §6.5): never inside OneDrive, an
// owner-only protected DACL, FILE_ATTRIBUTE_NOT_CONTENT_INDEXED (Windows Search), and the .nosync and
// .metadata_never_index markers. Shared by the depth/IR recorder (recorder.cpp, unchanged behaviour)
// and the camera roll (roll_store.cpp), which live in different libraries.
#pragma once

#include <string>
#include <vector>

namespace kvhost {

// True when path is inside one of the OneDrive roots (%OneDrive%, %OneDriveConsumer%, %OneDriveCommercial%,
// or the ones passed in, for tests).
bool isUnderOneDrive(const std::wstring& path, const std::vector<std::wstring>& extra_roots = {});

// Creates the folder (and its parents), applies the owner-only DACL and NOT_CONTENT_INDEXED, and writes
// the .nosync / .metadata_never_index markers for parity with the Mac. It doesn't check OneDrive: each
// caller refuses that first, with its own message.
bool protectPrivateFolder(const std::wstring& dir, std::string* err);

// The full path, lower-cased, without a trailing separator, for comparing folders.
std::wstring fullPathLower(const std::wstring& p);

} // namespace kvhost
