/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <errno.h>
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include "defs.h"

DiskManager::DiskManager() { memset(fd2pageno_, 0, MAX_FD * (sizeof(std::atomic<page_id_t>) / sizeof(char))); }

void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    int written = 0;
    while (written < num_bytes) {
        ssize_t bytes_write = pwrite(fd, offset + written, num_bytes - written, file_offset + written);
        if (bytes_write < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_write <= 0) {
            throw InternalError("DiskManager::write_page Error");
        }
        written += static_cast<int>(bytes_write);
    }
}

void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    int read_bytes = 0;
    while (read_bytes < num_bytes) {
        ssize_t bytes_read = pread(fd, offset + read_bytes, num_bytes - read_bytes, file_offset + read_bytes);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read < 0) {
            throw InternalError("DiskManager::read_page Error");
        }
        if (bytes_read == 0) {
            break;
        }
        read_bytes += static_cast<int>(bytes_read);
    }
    if (read_bytes < num_bytes) {
        memset(offset + read_bytes, 0, num_bytes - read_bytes);
    }
}

page_id_t DiskManager::allocate_page(int fd) {
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    std::string cmd = "mkdir " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string &path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

bool DiskManager::is_file(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void DiskManager::create_file(const std::string &path) {
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    #ifdef _WIN32
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_BINARY, 0664);
#else
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0664);
#endif
    if (fd < 0) {
        throw UnixError();
    }
    close(fd);
}

void DiskManager::destroy_file(const std::string &path) {
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path)) {
        throw FileNotClosedError(path);
    }
    if (unlink(path.c_str()) < 0) {
        throw UnixError();
    }
}

int DiskManager::open_file(const std::string &path) {
    if (path2fd_.count(path)) {
        throw FileExistsError(path);
    }
    #ifdef _WIN32
    int fd = open(path.c_str(), O_RDWR | O_BINARY);
#else
    int fd = open(path.c_str(), O_RDWR);
#endif
    if (fd < 0) {
        throw FileNotFoundError(path);
    }
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    return fd;
}

void DiskManager::close_file(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    std::string path = fd2path_[fd];
    fd2path_.erase(fd);
    path2fd_.erase(path);
    if (::close(fd) < 0) {
        throw UnixError();
    }
}

int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

int DiskManager::get_file_fd(const std::string &file_name) {
    if (!path2fd_.count(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}

int DiskManager::read_log(char *log_data, int size, int offset) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if(size == 0) return 0;
    int read_bytes = 0;
    while (read_bytes < size) {
        ssize_t bytes_read = pread(log_fd_, log_data + read_bytes, size - read_bytes, offset + read_bytes);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read < 0) {
            throw UnixError();
        }
        if (bytes_read == 0) {
            break;
        }
        read_bytes += static_cast<int>(bytes_read);
    }
    return read_bytes;
}

void DiskManager::write_log(char *log_data, int size) {
    std::lock_guard<std::mutex> lock(log_latch_);
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    struct stat stat_buf;
    if (fstat(log_fd_, &stat_buf) < 0) {
        throw UnixError();
    }
    off_t offset = stat_buf.st_size;
    int written = 0;
    while (written < size) {
        ssize_t bytes_write = pwrite(log_fd_, log_data + written, size - written, offset + written);
        if (bytes_write < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_write <= 0) {
            throw UnixError();
        }
        written += static_cast<int>(bytes_write);
    }
}


