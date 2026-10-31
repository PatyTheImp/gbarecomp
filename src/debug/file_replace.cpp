#include "file_replace.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gbarecomp::debug {
namespace {
std::string display_path(const std::filesystem::path& path) {
    auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}
bool fault(const char* phase) {
#ifdef GBARECOMP_FILE_REPLACE_TEST
    return file_replace_test_fault(phase);
#else
    return false;
#endif
}
}  // namespace

bool replace_file(const std::filesystem::path& path, const uint8_t* bytes,
                  std::size_t size, std::string* err) {
    auto tmp = path;
    tmp += ".tmp";
    auto fail = [&](const char* step, std::error_code ec, bool complete) {
        if (err) *err = std::string("file replacement ") + step + " failed: " +
                        ec.message() + "; destination preserved: " + display_path(path) +
                        (complete ? "; complete temporary retained: " + display_path(tmp) : "");
        return false;
    };
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) return fail("create directory", ec, false);
    }
#ifdef _WIN32
    auto last_error = [] { return std::error_code(GetLastError(), std::system_category()); };
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return fail("open temporary", last_error(), false);
    bool ok = true;
    std::size_t offset = 0;
    while (offset < size) {
        DWORD count = static_cast<DWORD>(std::min<std::size_t>(size - offset, 0x40000000u));
        bool inject = fault("write");
        if (inject) count = std::max<DWORD>(1, count / 2);
        DWORD written = 0;
        if (!WriteFile(h, bytes + offset, count, &written, nullptr)) {
            ec = last_error(); ok = false; break;
        }
        if (inject || written != count) {
            ec = std::make_error_code(std::errc::io_error); ok = false; break;
        }
        offset += written;
    }
    if (ok) {
        if (!FlushFileBuffers(h)) { ec = last_error(); ok = false; }
        else if (fault("flush")) { ec = std::make_error_code(std::errc::io_error); ok = false; }
    }
    // Close even after a failed write or flush; never rely on a destructor.
    bool closed = CloseHandle(h) != 0;
    if (!closed) {
        if (ok) ec = last_error();
        ok = false;
    }
    if (fault("close")) {
        if (ok) ec = std::make_error_code(std::errc::io_error);
        ok = false;
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return fail("write/flush/close temporary", ec, false);
    }
    fault("tmp_written");
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail("rename", last_error(), true);
#else
    // An exclusive unique temporary also prevents concurrent POSIX writers
    // from modifying the same inode after another writer has renamed it.
    std::string pattern = tmp.string() + ".XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = ::mkstemp(name.data());
    if (fd < 0) return fail("open temporary", {errno, std::generic_category()}, false);
    tmp = name.data();
    bool ok = true;
    std::size_t offset = 0;
    while (offset < size) {
        auto count = std::min<std::size_t>(size - offset, 0x40000000u);
        bool inject = fault("write");
        if (inject) count = std::max<std::size_t>(1, count / 2);
        auto written = ::write(fd, bytes + offset, count);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0 || inject) {
            ec = {written < 0 ? errno : EIO, std::generic_category()}; ok = false; break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && (::fsync(fd) != 0 || fault("flush"))) {
        ec = std::make_error_code(std::errc::io_error); ok = false;
    }
    bool closed = ::close(fd) == 0;
    if (!closed || fault("close")) {
        if (ok) ec = std::make_error_code(std::errc::io_error);
        ok = false;
    }
    if (!ok) {
        ::unlink(tmp.c_str());
        return fail("write/flush/close temporary", ec, false);
    }
    fault("tmp_written");
    if (::rename(tmp.c_str(), path.c_str()) != 0)
        return fail("rename", {errno, std::generic_category()}, true);
    // Persist the directory entry after the atomic rename. Failure here cannot
    // undo the replacement; the destination already contains complete bytes.
    auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    int dir = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir < 0) {
        if (err) *err = "replacement complete; cannot open parent for durability sync";
        return false;
    }
    bool synced = ::fsync(dir) == 0;
    bool dir_closed = ::close(dir) == 0;
    if (!synced || !dir_closed) {
        if (err) *err = "replacement complete; parent directory sync/close failed";
        return false;
    }
#endif
    return true;
}
}  // namespace gbarecomp::debug
