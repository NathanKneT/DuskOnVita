// Compiled only, under -Werror=frame-larger-than=8192: passes with TARGET_VITA=1
// (patch 0005 active) and must fail with TARGET_VITA=0 (original 64 KiB buffer).
#include <array>
#include <cstddef>
#include <cstdint>
struct SDL_IOStream;
enum SDL_IOStatus { SDL_IO_STATUS_READY, SDL_IO_STATUS_EOF, SDL_IO_STATUS_ERROR };
size_t SDL_ReadIO(SDL_IOStream*, void*, size_t);
size_t SDL_WriteIO(SDL_IOStream*, const void*, size_t);
SDL_IOStatus SDL_GetIOStatus(SDL_IOStream*);
bool SDL_InvalidParamError(const char*);
#include "candidate.inc"
