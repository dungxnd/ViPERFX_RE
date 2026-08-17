// Shim: minimal RAII file-descriptor wrapper (mirrors android-base/unique_fd.h API)
#pragma once
#include <unistd.h>
namespace android {
namespace base {

struct unique_fd {
    unique_fd() = default;
    explicit unique_fd(int fd) : fd_(fd) {}
    unique_fd(unique_fd &&o) noexcept : fd_(o.release()) {}
    ~unique_fd() { if (fd_ >= 0) ::close(fd_); }
    unique_fd &operator=(unique_fd &&o) noexcept { reset(o.release()); return *this; }

    int  get()     const { return fd_; }
    int  release()       { int f = fd_; fd_ = -1; return f; }
    void reset(int f=-1) { if (fd_ >= 0) ::close(fd_); fd_ = f; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    unique_fd(const unique_fd &) = delete;
    unique_fd &operator=(const unique_fd &) = delete;
};

} // namespace base
} // namespace android
