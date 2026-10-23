cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(twilight_vita LANGUAGES C CXX)

if (NOT VITA)
    message(FATAL_ERROR "Configure with the VitaSDK toolchain (tools/vita_build.sh does this)")
endif ()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
# Static ELF only: no PIC (upstream turns it on for its shared-library platforms).
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# Enum ABI (docs/ARCHITECTURE.md, "Enum ABI"): arm-vita-eabi defaults to
# short enums (__ARM_SIZEOF_MINIMAL_ENUM 1). The decomp, Aurora and Dusklight
# assume int-sized enums (GameCube -enum int, every desktop/mobile ABI; the mod
# API static_asserts it), so everything built from source here uses
# -fno-short-enums. Only platform/vita/sdk code, which includes psp2/SDL3
# headers whose prebuilt libraries use short enums, is built with -fshort-enums,
# and it exchanges no enum-typed data with the rest.
add_compile_options(-fno-short-enums -ffunction-sections -fdata-sections)
add_link_options(-Wl,--no-enum-size-warning -Wl,--gc-sections)

include(cmake/TwvDeps.cmake)
include(cmake/DusklightSources.cmake)

# Milestone 0 aggregate: every game TU compiles for arm-vita-eabi.
add_custom_target(twv_m0_game_libs DEPENDS twv_game ${JSYSTEM_LIBRARIES})

# Milestone 1: whole-graph link with generated trap stubs -> eboot.bin / VPK.
include(cmake/VitaApp.cmake)
