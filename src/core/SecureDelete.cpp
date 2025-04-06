#include "../../include/zsh_history_cleaner/SecureDelete.h"
#include "../../include/zsh_history_cleaner/Constants.h" // For SHRED_PASSES, TMP_PREFIX, SHRED_BUFFER_SIZE
#include "../../include/zsh_history_cleaner/Utils.h"     // For nowEpoch()

#include <iostream>    // For std::cerr, std::endl, std::ostream
#include <vector>
#include <random>
#include <algorithm>   // For std::min, std::fill
#include <system_error>// For std::error_code
#include <cerrno>      // For errno
#include <cstring>     // For strerror
#include <unistd.h>    // For open, write, lseek, fsync, close, unlink, truncate
#include <fcntl.h>     // For O_WRONLY, O_SYNC
#include <sys/stat.h>  // For S_ISREG (though filesystem::is_regular_file is used)

namespace fs = std::filesystem;

// Best-effort secure delete implementation
bool secureDelete(const fs::path& filepath, int passes, std::ostream& log) {
    // RAII class for file descriptor management
    class FileHandle {
    public:
        explicit FileHandle(int fd) : fd_(fd) {}
        ~FileHandle() { if (fd_ != -1) close(fd_); }
        
        int get() const { return fd_; }
        bool isValid() const { return fd_ != -1; }
        
        // Prevent copying
        FileHandle(const FileHandle&) = delete;
        FileHandle& operator=(const FileHandle&) = delete;
        // Allow moving
        FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
        FileHandle& operator=(FileHandle&& other) noexcept {
            if (this != &other) {
                if (fd_ != -1) close(fd_);
                fd_ = other.fd_;
                other.fd_ = -1;
            }
            return *this;
        }
    private:
        int fd_;
    };

    // RAII class for secure buffer management
    class SecureBuffer {
    public:
        explicit SecureBuffer(size_t size) : buffer_(size) {}
        ~SecureBuffer() {
            // Secure cleanup - overwrite buffer before destruction
            std::fill(buffer_.begin(), buffer_.end(), 0);
        }
        
        unsigned char* data() { return buffer_.data(); }
        size_t size() const { return buffer_.size(); }
        
        void fillPattern(unsigned char pattern) {
            std::fill(buffer_.begin(), buffer_.end(), pattern);
        }
        
        void fillRandom(std::mt19937& gen, std::uniform_int_distribution<unsigned int>& dist) {
            for (auto& byte : buffer_) {
                byte = static_cast<unsigned char>(dist(gen));
            }
        }
        
    private:
        std::vector<unsigned char> buffer_;
    };

    // 1. Validate file
    std::error_code ec;
    if (!fs::is_regular_file(filepath, ec)) {
        if (ec) {
            log << "Error: Failed to check file status: " << ec.message() << std::endl;
            return false;
        }
        if (fs::exists(filepath)) {
            log << "Warning: Not a regular file, using standard delete for: " << filepath << std::endl;
            return fs::remove(filepath, ec);
        }
        log << "Info: File already deleted: " << filepath << std::endl;
        return true;
    }

    // 2. Get file size
    uintmax_t fileSize = fs::file_size(filepath, ec);
    if (ec) {
        log << "Error: Failed to get file size: " << ec.message() << std::endl;
        return fs::remove(filepath, ec);
    }

    if (fileSize == 0) {
        log << "Info: Empty file, using standard delete" << std::endl;
        return fs::remove(filepath, ec);
    }

    // 3. Open file with sync flags
    FileHandle fd(open(filepath.c_str(), O_WRONLY | O_SYNC));
    if (!fd.isValid() && (errno == EINVAL || errno == EOPNOTSUPP)) {
        log << "Info: O_SYNC not supported, falling back to standard write mode" << std::endl;
        fd = FileHandle(open(filepath.c_str(), O_WRONLY));
    }
    
    if (!fd.isValid()) {
        log << "Error: Failed to open file: " << std::strerror(errno) << std::endl;
        return fs::remove(filepath, ec);
    }

    // 4. Initialize secure buffer and RNG
    SecureBuffer buffer(SHRED_BUFFER_SIZE);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);


    // 5. Perform overwrite passes
    for (int pass = 1; pass <= passes; ++pass) {
        if (lseek(fd.get(), 0, SEEK_SET) == -1) {
            log << "Error: Failed to seek to start of file: " << std::strerror(errno) << std::endl;
            return fs::remove(filepath, ec);
        }

        // Always fill with random data
        buffer.fillRandom(gen, dist);

        // Write pattern to file
        uintmax_t remaining = fileSize;
        while (remaining > 0) {
            size_t writeSize = std::min(static_cast<uintmax_t>(buffer.size()), remaining);
            ssize_t written = write(fd.get(), buffer.data(), writeSize);

            if (written == -1) {
                if (errno == EINTR) continue;
                log << "Error: Write failed: " << std::strerror(errno) << std::endl;
                return fs::remove(filepath, ec);
            }
            if (static_cast<size_t>(written) != writeSize) {
                log << "Error: Incomplete write" << std::endl;
                return fs::remove(filepath, ec);
            }
            remaining -= written;
        }

        // Sync after each pass
        if (fsync(fd.get()) == -1) {
            log << "Warning: fsync failed: " << std::strerror(errno) << std::endl;
        }
    }

    // 6. Rename before delete - use random string for filename
    // Generate random filename
    std::string randomStr;
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 15; ++i) {
        randomStr += charset[dist(gen) % (sizeof(charset) - 1)];
    }
    fs::path obscurePath = filepath.parent_path() / randomStr;
    ec.clear();
    fs::rename(filepath, obscurePath, ec);
    if (ec) {
        obscurePath = filepath;
    }

    // 8. Final deletion
    if (!fs::remove(obscurePath, ec)) {
        log << "Error: Failed to delete file: " << ec.message() << std::endl;
        return false;
    }

    return true;
}