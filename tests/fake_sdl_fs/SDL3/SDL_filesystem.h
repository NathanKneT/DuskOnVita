#pragma once
// Host stand-in for SDL3's file system calls (see SDL_iostream.h here).
#include "SDL_iostream.h"

typedef enum SDL_PathType { SDL_PATHTYPE_NONE, SDL_PATHTYPE_FILE, SDL_PATHTYPE_DIRECTORY, SDL_PATHTYPE_OTHER } SDL_PathType;
typedef struct SDL_PathInfo {
    SDL_PathType type;
    Uint64 size;
    SDL_Time create_time;
    SDL_Time modify_time;
    SDL_Time access_time;
} SDL_PathInfo;

extern "C" {
bool SDL_RenamePath(const char* oldpath, const char* newpath);
bool SDL_RemovePath(const char* path);
bool SDL_CreateDirectory(const char* path);
bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info);
char* SDL_GetPrefPath(const char* org, const char* app);
}
