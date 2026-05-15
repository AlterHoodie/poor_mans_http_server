#pragma once

#include <unistd.h>

struct unique_fd{
    private:
        int fd_;
    public:
        explicit unique_fd(int fd =-1) noexcept : fd_(fd){};

        // No Copying (no copy constructors) only moving
        unique_fd(const unique_fd&) = delete;
        unique_fd& operator=(const unique_fd&) = delete;

        // Move constructor / Move assignment
        unique_fd(unique_fd&& other) noexcept : fd_(other.fd_) {other.fd_ = -1;}
        unique_fd& operator=(unique_fd&& other) noexcept {
            if (this != &other) {
                reset();
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }

        ~unique_fd() { reset(); }

        [[nodiscard]] int get() const noexcept {return fd_;};
        [[nodiscard]] int release() noexcept{
            int t = fd_;
            fd_ = -1;
            return t;
        }

        void reset(int new_fd = -1) noexcept{
            if (fd_ >=0) :: close(fd_);
            fd_ = new_fd;
        }
};