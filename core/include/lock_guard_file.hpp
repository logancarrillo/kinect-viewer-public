#pragma once

#include <string>

namespace kv {

class KinectLockFile {
public:
    explicit KinectLockFile(std::string lock_path = "");
    ~KinectLockFile();

    KinectLockFile(const KinectLockFile&) = delete;
    KinectLockFile& operator=(const KinectLockFile&) = delete;

    const std::string& path() const { return path_; }

    // Returns true if the lock file exists and is NOT owned by this process.
    bool isLockedByOther(std::string* out_owner_info = nullptr) const;

    // Atomically acquires the lock file if free (or already owned by this process).
    bool acquire(std::string* out_error = nullptr);

    // Releases the lock file if owned by this process.
    void release();

    bool isHeld() const { return held_; }

    static std::string defaultLockPath();

private:
    std::string path_;
    bool held_ = false;

    bool isOwnedByCurrentProcess(const std::string& content) const;
};

} // namespace kv
