cmake_minimum_required(VERSION 3.25)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif ()

project(twilight_vita LANGUAGES C CXX)

if (NOT VITA)
    message(FATAL_ERROR "Configure with the VitaSDK toolchain (tools/vita_build.sh does this)")
endif ()

# Optimisation of the RelWithDebInfo build. O2 (-O2 -g -DNDEBUG) is the default and the
# reference; O3 and the LTO variants are opt-in perf builds (tools/vita_perf_build.sh
# build-dir -DTWV_OPT=O3). None of them changes floating-point results: no -ffast-math,
# -ffp-contract stays GCC's default (identical at -O2 and -O3), the Cortex-A9 VFPv3 has no
# fused multiply-add, and GCC never vectorises float arithmetic onto ARMv7 NEON (flush to
# zero) without -funsafe-math-optimizations. -g is kept for crash symbols.
set(TWV_OPT "O2" CACHE STRING "Optimisation of the RelWithDebInfo build: O2, O3, O2LTO or O3LTO")
set_property(CACHE TWV_OPT PROPERTY STRINGS O2 O3 O2LTO O3LTO)
if (NOT TWV_OPT MATCHES "^(O2|O3|O2LTO|O3LTO)$")
    message(FATAL_ERROR "TWV_OPT must be O2, O3, O2LTO or O3LTO (got '${TWV_OPT}')")
endif ()
# -O3 also exploits out-of-bounds loops of the decompiled code that -O2 leaves alone: with
# loop splitting, d_a_e_rd.cpp's 11-joint loop writes field_0xabc[4] up to index 10, and GCC
# 15 bounds the loop to its first valid iteration (-Waggressive-loop-optimizations), which
# would freeze joints 4-10 of the Bulblin rider. -fno-aggressive-loop-optimizations keeps
# every such loop running its written iteration count (as the original does). -O2 needs it
# too: it already cut d_a_e_tk.cpp's mActionTimer[3] loop to 3 passes instead of the
# original's 4, so the flag applies at every level (retail behaviour).
foreach (_lang C CXX)
    string(APPEND CMAKE_${_lang}_FLAGS " -fno-aggressive-loop-optimizations")
endforeach ()
if (TWV_OPT MATCHES "^O3")
    foreach (_lang C CXX)
        string(REGEX REPLACE "(^| )-O2( |$)" "\\1-O3\\2"
                CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${_lang}_FLAGS_RELWITHDEBINFO}")
    endforeach ()
endif ()
set(TWV_LTO OFF)
if (TWV_OPT MATCHES "LTO$")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _twv_ipo OUTPUT _twv_ipo_log LANGUAGES C CXX)
    if (NOT _twv_ipo)
        message(FATAL_ERROR "TWV_OPT=${TWV_OPT}: the toolchain reports no LTO support: ${_twv_ipo_log}")
    endif ()
    # Targets created from here on (game, platform, fmt, abseil) compile to GCC IR;
    # vitaGL and the SDK's prebuilt libraries stay regular objects. VitaApp.cmake keeps
    # the translation units that define --wrap'd symbols out of LTO.
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    set(TWV_LTO ON)
endif ()
message(STATUS "TWV_OPT=${TWV_OPT}: C++ ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}, LTO ${TWV_LTO}")

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
