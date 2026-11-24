// Small file helpers for the pet-camera state files next to pairing.json (pet-camera.md §5):
// each file is owner-only and replaced atomically, the same way saveHostStateAtomically does it (the connection
// log is also appended to between those writes).
#pragma once

#include <string>

namespace kvhost {

// Makes dir owner-only (creating it if needed), writes <dir>\<name>.tmp, gives it the owner-only
// DACL, then replaces <dir>\<name> with MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH).
bool writeOwnerOnlyFileAtomically(const std::wstring& dir, const std::wstring& name, const std::string& text,
                                  std::string* err = nullptr);

// Appends text to a file that is already there, then flushes it. It never creates one, so a state file is only ever
// made by writeOwnerOnlyFileAtomically, with its owner-only access list. False (with err) when the file can't be
// opened for appending or the write fails; part of the text may then be in the file.
bool appendToExistingFile(const std::wstring& path, const std::string& text, std::string* err = nullptr);

// Reads the whole file. False when it is missing or can't be read.
bool readWholeFile(const std::wstring& path, std::string& out);

// What GetFileAttributesW says about a state file. Only "not found" (ERROR_FILE_NOT_FOUND or
// ERROR_PATH_NOT_FOUND) is Missing. Any other failure, or a folder where the file should be, is Unknown: the
// file may well be there, so a caller takes it as there but unreadable (fail closed). win32err gets the
// error (0 for Present).
enum class FileState { Missing, Present, Unknown };
FileState fileState(const std::wstring& path, unsigned long* win32err = nullptr);

// Keeps a copy of <dir>\<name> for a human to look at. The newest copy is <name>.bad; older ones move up to
// <name>.bad.1 ... <name>.bad.4, and the oldest beyond those is dropped, so a later bad start never overwrites an
// earlier copy (such as the old identity a reset kept). Nothing is copied when one of the kept copies already holds
// the same bytes. The copy is made to <name>.bad.tmp first, so a copy that fails moves nothing. what: a phrase for
// an error text ("kept as <name>.bad", "<name>.bad.2 already held it", or "keeping a copy as <name>.bad failed
// (Win32 error n)"). False when no kept copy holds the file's current bytes.
constexpr int kBadCopiesKept = 5;
bool keepBadCopy(const std::wstring& dir, const std::wstring& name, std::string* what = nullptr);

// fileState(path) == Present.
bool fileExists(const std::wstring& path);

// Test seam: while set, fileState asks this instead of GetFileAttributesW. It returns the attributes, or
// INVALID_FILE_ATTRIBUTES with *win32err set. nullptr goes back to the real call.
using FileAttributesProbe = unsigned long (*)(const std::wstring& path, unsigned long* win32err);
void setFileAttributesProbeForTest(FileAttributesProbe probe);

} // namespace kvhost
