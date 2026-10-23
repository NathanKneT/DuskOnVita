# Third-party libraries for the Vita build, from sources unpacked by
# tools/fetch_deps.sh into TWV_DEPS_DIR (pinned in deps.lock) or from the
# digest-pinned VitaSDK image (SDL3, zstd, libpng, zlib).
include_guard(GLOBAL)

if (NOT TWV_DEPS_DIR OR NOT EXISTS "${TWV_DEPS_DIR}/tracy")
    message(FATAL_ERROR "TWV_DEPS_DIR must point at the output of tools/fetch_deps.sh")
endif ()

# Tracy: headers only. TRACY_ENABLE stays undefined, so every zone macro is a no-op.
add_library(twv_tracy INTERFACE)
target_include_directories(twv_tracy SYSTEM INTERFACE "${TWV_DEPS_DIR}/tracy/public")

add_library(twv_json INTERFACE)
target_include_directories(twv_json SYSTEM INTERFACE "${TWV_DEPS_DIR}/json/include")
add_library(twv_cxxopts INTERFACE)
target_include_directories(twv_cxxopts SYSTEM INTERFACE "${TWV_DEPS_DIR}/cxxopts/include")
add_library(twv_picosha2 INTERFACE)
target_include_directories(twv_picosha2 SYSTEM INTERFACE "${TWV_DEPS_DIR}/picosha2")

add_library(twv_xxhash STATIC "${TWV_DEPS_DIR}/xxhash/xxhash.c")
target_include_directories(twv_xxhash SYSTEM PUBLIC "${TWV_DEPS_DIR}/xxhash")

add_library(twv_miniz STATIC "${TWV_DEPS_DIR}/miniz/miniz.c")
target_include_directories(twv_miniz SYSTEM PUBLIC "${TWV_DEPS_DIR}/miniz")

# fmt 12.1.0 (Aurora's pin). FMT_OS pulls in POSIX file APIs we do not need.
set(FMT_OS OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${TWV_DEPS_DIR}/fmt" "${CMAKE_BINARY_DIR}/_deps/fmt" EXCLUDE_FROM_ALL SYSTEM)

# abseil 20240722.0 (Aurora's standalone pin); only the hash containers are used.
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${TWV_DEPS_DIR}/abseil" "${CMAKE_BINARY_DIR}/_deps/abseil" EXCLUDE_FROM_ALL SYSTEM)

find_package(SDL3 CONFIG REQUIRED)
find_package(PNG REQUIRED)
find_package(zstd CONFIG REQUIRED)
