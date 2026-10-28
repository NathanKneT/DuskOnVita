// Host stand-in for the SDL3 calls in tests/fake_sdl_fs/SDL3: stdio streams and POSIX paths.
// SDL_RenamePath replaces an existing target, as SDL does on POSIX systems.
#include <SDL3/SDL_filesystem.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>

struct SDL_IOStream {
    std::FILE* file;
    SDL_IOStatus status;
};

namespace {
std::string gError;
}

extern "C" {
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    if (file == nullptr || mode == nullptr) {
        SDL_InvalidParamError("file");
        return nullptr;
    }
    std::FILE* handle = std::fopen(file, mode);
    if (handle == nullptr) {
        SDL_SetError("Couldn't open %s: %s", file, std::strerror(errno));
        return nullptr;
    }
    return new SDL_IOStream{handle, SDL_IO_STATUS_READY};
}

size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size) {
    if (context == nullptr) return 0;
    const size_t read = std::fread(ptr, 1, size, context->file);
    if (read < size) context->status = std::feof(context->file) ? SDL_IO_STATUS_EOF : SDL_IO_STATUS_ERROR;
    return read;
}

size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size) {
    if (context == nullptr) return 0;
    const size_t written = std::fwrite(ptr, 1, size, context->file);
    if (written < size) context->status = SDL_IO_STATUS_ERROR;
    return written;
}

bool SDL_CloseIO(SDL_IOStream* context) {
    if (context == nullptr) return true;
    const bool closed = std::fclose(context->file) == 0;
    delete context;
    return closed;
}

Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence) {
    const int origin = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (context == nullptr || std::fseek(context->file, static_cast<long>(offset), origin) != 0) return -1;
    context->status = SDL_IO_STATUS_READY;
    return std::ftell(context->file);
}

Sint64 SDL_GetIOSize(SDL_IOStream* context) {
    if (context == nullptr) return -1;
    const long here = std::ftell(context->file);
    std::fseek(context->file, 0, SEEK_END);
    const long size = std::ftell(context->file);
    std::fseek(context->file, here, SEEK_SET);
    return size;
}

bool SDL_FlushIO(SDL_IOStream* context) { return context != nullptr && std::fflush(context->file) == 0; }
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* context) { return context ? context->status : SDL_IO_STATUS_ERROR; }

bool SDL_SetError(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    gError = buffer;
    return false;
}

const char* SDL_GetError(void) { return gError.c_str(); }
Uint32 SDL_rand_bits(void) { return static_cast<Uint32>(std::rand()); }
void SDL_free(void* mem) { std::free(mem); }

bool SDL_RenamePath(const char* oldpath, const char* newpath) {
    if (std::rename(oldpath, newpath) != 0) return SDL_SetError("Can't rename %s: %s", oldpath, std::strerror(errno));
    return true;
}

bool SDL_RemovePath(const char* path) {
    if (std::remove(path) != 0) return SDL_SetError("Can't remove %s: %s", path, std::strerror(errno));
    return true;
}

bool SDL_CreateDirectory(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (!std::filesystem::is_directory(path, ec)) return SDL_SetError("Can't create %s", path);
    return true;
}

bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info) {
    struct stat st {};
    if (::stat(path, &st) != 0) return SDL_SetError("Can't stat %s", path);
    if (info != nullptr) {
        *info = {};
        info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : SDL_PATHTYPE_OTHER;
        info->size = static_cast<Uint64>(st.st_size);
    }
    return true;
}

char* SDL_GetPrefPath(const char*, const char*) {
    SDL_SetError("no preference path in the host tests");
    return nullptr;
}
}
