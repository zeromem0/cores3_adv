/*
 * Just enough of the Arduino API for the emulator core to compile.
 *
 * The Z80 and Spectrum sources come from an Arduino sketch, while this
 * firmware is plain ESP-IDF. Rather than editing every call site -- which
 * would make re-syncing with upstream painful -- the handful of Arduino
 * things they actually use are provided here: logging and file reading.
 *
 * The card is mounted by the HAL at /sdcard, so files go through stdio.
 */
#pragma once

#include <dirent.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

/* A function rather than Arduino's macro: these headers are pulled in
 * alongside the standard library, and a function-like max() macro breaks
 * every std::numeric_limits<>::max() it reaches. */
template <typename T>
static inline T max(T a, T b)
{
    return (a > b) ? a : b;
}

/* -------------------------------------------------------------------------- */
/*                                   Logging                                  */
/* -------------------------------------------------------------------------- */
class SerialCompat {
public:
    void begin(unsigned long = 0)
    {
        /* The console is already open; a sketch asking for a baud rate
         * is asking for something this firmware settled at start-up. */
    }

    void printf(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    void println(const char* text = "")
    {
        ::printf("%s\n", text);
    }
    void print(const char* text)
    {
        ::printf("%s", text);
    }
};

static SerialCompat Serial;

/* -------------------------------------------------------------------------- */
/*                                    Files                                   */
/* -------------------------------------------------------------------------- */
#define FILE_READ "rb"
#define FILE_WRITE "wb"

class File {
public:
    File() = default;
    explicit File(std::FILE* handle, const std::string& path = "") : _handle(handle), _path(path) {}

    /* A directory, which stdio does not open as a file: it is walked with
     * openNextFile() instead. */
    explicit File(DIR* dir, const std::string& path) : _dir(dir), _path(path) {}

    explicit operator bool() const
    {
        return _handle != nullptr || _dir != nullptr;
    }

    size_t read(uint8_t* buffer, size_t length)
    {
        return _handle ? std::fread(buffer, 1, length, _handle) : 0U;
    }

    size_t write(const uint8_t* buffer, size_t length)
    {
        return _handle ? std::fwrite(buffer, 1, length, _handle) : 0U;
    }

    /*
     * The file's own name, without the directories above it. Sketches
     * written against the Arduino library put the folder back on
     * themselves -- "/ZXgames/" + name -- so handing back a full path
     * gives them the folder twice.
     */
    const char* name() const
    {
        const size_t slash = _path.find_last_of('/');
        return (slash == std::string::npos) ? _path.c_str() : _path.c_str() + slash + 1;
    }

    bool isDirectory() const
    {
        return _dir != nullptr;
    }

    /** @brief The next entry of a directory, or a closed file at the end. */
    File openNextFile();

    size_t size()
    {
        if (!_handle) {
            return 0U;
        }
        const long here = std::ftell(_handle);
        std::fseek(_handle, 0, SEEK_END);
        const long end = std::ftell(_handle);
        std::fseek(_handle, here, SEEK_SET);
        return (size_t)end;
    }

    size_t position()
    {
        return _handle ? (size_t)std::ftell(_handle) : 0U;
    }

    bool seek(size_t offset)
    {
        return _handle && std::fseek(_handle, (long)offset, SEEK_SET) == 0;
    }

    size_t available()
    {
        return _handle ? size() - position() : 0U;
    }

    void close()
    {
        if (_handle) {
            std::fclose(_handle);
            _handle = nullptr;
        }
        if (_dir) {
            closedir(_dir);
            _dir = nullptr;
        }
    }

private:
    std::FILE* _handle = nullptr;
    DIR* _dir          = nullptr;
    std::string _path;
};

inline File File::openNextFile()
{
    if (_dir == nullptr) {
        return File();
    }
    const struct dirent* entry = readdir(_dir);
    if (entry == nullptr) {
        return File();
    }

    /* The name is given whole, as the Arduino library does: the sketches
     * that use this take the part after the last slash themselves. */
    const std::string path = _path + "/" + entry->d_name;
    return File(std::fopen(path.c_str(), "rb"), path);
}

class SDCompat {
public:
    /* Where the HAL mounts the card. Sketches written for the Arduino
     * library address it from the root -- "/ZXgames/..." -- so a path
     * that does not already start here is placed under it. */
    static constexpr const char* kMount = "/sdcard";

    static std::string resolve(const std::string& path)
    {
        if (path.compare(0, std::strlen(kMount), kMount) == 0) {
            return path;
        }
        return std::string(kMount) + (path.empty() || path[0] == '/' ? "" : "/") + path;
    }

    File open(const std::string& path, const char* mode = FILE_READ)
    {
        const std::string target = resolve(path);

        /* A directory has no stdio handle, so it is opened as one and
         * walked with openNextFile(). */
        struct stat info = {};
        if (stat(target.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            return File(opendir(target.c_str()), target);
        }
        return File(std::fopen(target.c_str(), mode), target);
    }

    bool exists(const std::string& path)
    {
        struct stat info = {};
        return stat(resolve(path).c_str(), &info) == 0;
    }

    bool mkdir(const std::string& path)
    {
        return exists(path) || ::mkdir(resolve(path).c_str(), 0777) == 0;
    }
};

static SDCompat SD;
