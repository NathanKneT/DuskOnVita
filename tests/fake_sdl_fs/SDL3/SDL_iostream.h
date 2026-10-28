#pragma once
// Host stand-in for the SDL3 stream, file system and error calls Aurora's card and io code use
// (tests/card_save_safety_test.cpp). Backed by stdio and POSIX in tests/fake_sdl_fs/fake_sdl_fs.cpp:
// files are real files, so the card code reads and writes a real folder.
#include <stddef.h>
#include <stdint.h>

typedef int8_t Sint8;
typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;
typedef Sint64 SDL_Time;

typedef struct SDL_IOStream SDL_IOStream;
typedef enum SDL_IOStatus {
    SDL_IO_STATUS_READY,
    SDL_IO_STATUS_ERROR,
    SDL_IO_STATUS_EOF,
    SDL_IO_STATUS_NOT_READY,
    SDL_IO_STATUS_READONLY,
    SDL_IO_STATUS_WRITEONLY
} SDL_IOStatus;
typedef enum SDL_IOWhence { SDL_IO_SEEK_SET, SDL_IO_SEEK_CUR, SDL_IO_SEEK_END } SDL_IOWhence;

extern "C" {
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode);
size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size);
size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size);
bool SDL_CloseIO(SDL_IOStream* context);
Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence);
Sint64 SDL_GetIOSize(SDL_IOStream* context);
bool SDL_FlushIO(SDL_IOStream* context);
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* context);
bool SDL_SetError(const char* fmt, ...);
const char* SDL_GetError(void);
Uint32 SDL_rand_bits(void);
void SDL_free(void* mem);
}
#define SDL_InvalidParamError(param) SDL_SetError("Parameter '%s' is invalid", (param))
