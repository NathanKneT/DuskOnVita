# Milestone 1: link the real game graph into a Vita executable.
#
#   twv_link_probe   main + runtime + game libraries, unresolved symbols allowed
#   gen_stubs.py     every strong undefined symbol -> named trap stub (or zeroed data)
#   twilight_vita    same inputs + generated stubs, strict link -> eboot.bin -> VPK
#
# The stub list (link_report.txt) is the measured porting backlog: Aurora, Borealis,
# Dusklight platform code and SDL3 that the Vita build does not provide yet.
include_guard(GLOBAL)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(TWV_TITLE_ID "TWVT00001")
set(TWV_VERSION "00.01")
option(TWV_VITA_RENDER_BUNDLE "Compile startup-selectable grouped Vita rendering optimizations" OFF)
option(TWV_VITA_GX_FAST_DECODE "Compile startup-selectable exact GX decode and cached shader-info reuse" OFF)

# SDK domain: psp2 headers and prebuilt SDK libraries, default short enums.
add_library(twv_vita_runtime STATIC
        platform/vita/runtime/telemetry.cpp
        platform/vita/runtime/system_probe.cpp
        platform/vita/runtime/stub_trap.cpp
        platform/vita/runtime/fault_receipts.cpp
        platform/vita/runtime/run_control.cpp
        platform/vita/runtime/gx_thread.cpp
        platform/vita/gfx_vgl/bundle_options.cpp
        platform/vita/gfx_vgl/gx_fast_control.cpp
        platform/vita/audio/audio_out.cpp)
set_property(SOURCE platform/vita/gfx_vgl/bundle_options.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS TWV_VITA_RENDER_BUNDLE=$<BOOL:${TWV_VITA_RENDER_BUNDLE}>)
set_property(SOURCE platform/vita/gfx_vgl/gx_fast_control.cpp APPEND PROPERTY
    COMPILE_DEFINITIONS TWV_VITA_GX_FAST_DECODE=$<BOOL:${TWV_VITA_GX_FAST_DECODE}>)
target_include_directories(twv_vita_runtime PUBLIC platform/vita)
target_compile_options(twv_vita_runtime PRIVATE -fshort-enums)
# abort() goes through fault_receipts.cpp first, from every object and archive.
target_link_options(twv_vita_runtime INTERFACE -Wl,--wrap=abort)
# The pinned Vita linker script explicitly supports __sce_headroom before its
# 64 KiB RW alignment. Reserve room for vita-elf-create's SCE metadata; otherwise
# innocuous text growth can leave <4820 bytes and overlap the next load segment.
target_link_options(twv_vita_runtime INTERFACE -Wl,--defsym=__sce_headroom=0x2000)
target_link_libraries(twv_vita_runtime PUBLIC
        SceLibKernel_stub SceIofilemgr_stub SceKernelThreadMgr_stub SceProcessmgr_stub SceSysmem_stub ScePower_stub
        SceAudio_stub)
set_source_files_properties(platform/vita/main_vita.cpp platform/vita/compat/newlib_gaps.c PROPERTIES COMPILE_OPTIONS -fshort-enums)

# Dusklight's own platform/UI layer (src/dusk: OS threads/queues, settings,
# config, audio DSP emulation, menus). Linked; what it needs from Aurora,
# Borealis, SDL3, RmlUi and ImGui is stubbed until milestone 2.
set(TWV_DUSK_AUDIO_FILES
        ${DUSK_ROOT}/src/dusk/audio/DuskAudioSystem.cpp
        ${DUSK_ROOT}/src/dusk/audio/JASCriticalSection.cpp
        ${DUSK_ROOT}/src/dusk/audio/DuskDsp.cpp
        ${DUSK_ROOT}/src/dusk/audio/Adpcm.cpp
        ${DUSK_ROOT}/src/dusk/audio/DspStub.cpp
        ${DUSK_ROOT}/src/dusk/imgui/ImGuiAudio.cpp)
# patches/dusklight/0004: the mods gfx service (WebGPU device access) is not built on the Vita.
list(REMOVE_ITEM DUSK_FILES "${DUSK_ROOT}/src/dusk/mods/svc/gfx.cpp")
add_library(twv_dusk STATIC ${DUSK_FILES} ${TWV_DUSK_AUDIO_FILES} platform/vita/dusk/mods_gfx_vita.cpp)
# patches/dusklight/0003: DuskAudioSystem.cpp includes the Vita output header (plain types only).
set_property(SOURCE ${DUSK_ROOT}/src/dusk/audio/DuskAudioSystem.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita")
target_link_libraries(twv_dusk PUBLIC twv_game_headers)
target_precompile_headers(twv_dusk PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SOURCE_DIR}/platform/vita/compat/twv_prelude.h>"
        "$<$<COMPILE_LANGUAGE:CXX>:${DUSK_ROOT}/include/dusk_pch.hpp>")

# GNU ld resolves static archives in one pass; the game and JSystem libraries
# reference each other (upstream uses the same RESCAN group). CMake only knows
# the feature for hosted platforms, so declare it for the Vita toolchain.
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)

# Aurora's matrix library (extern/aurora/cmake/aurora_mtx.cmake): plain C, no platform work.
add_library(twv_aurora_mtx STATIC
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtx.c
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtxstack.c
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtxvec.c
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/mtx44.c
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/vec.c
        ${DUSK_ROOT}/extern/aurora/lib/dolphin/mtx/quat.c)
target_include_directories(twv_aurora_mtx PUBLIC "${DUSK_ROOT}/extern/aurora/include")
target_compile_definitions(twv_aurora_mtx PRIVATE MTX_USE_PS=1) # as Dusklight's CMakeLists.txt sets it

# In-tree freeverb (Dusklight's reverb for the DSP emulation); its own CMakeLists.
add_subdirectory("${DUSK_ROOT}/libs/freeverb" "${CMAKE_BINARY_DIR}/_deps/freeverb" EXCLUDE_FROM_ALL)

# Milestone 2: Borealis, Aurora (null gfx), the disc reader.
include(cmake/PlatformLibs.cmake)
if(TWV_VITA_GX_FAST_DECODE AND NOT TWV_VITA_GFX_VGL)
    message(FATAL_ERROR "TWV_VITA_GX_FAST_DECODE requires the VitaGL backend")
endif()

option(TWV_VITA_BENCHMARK "Build opt-in isolated original-input combat fixture (disabled without explicit args)" OFF)
if(TWV_VITA_BENCHMARK)
    target_sources(twv_game PRIVATE platform/vita/benchmark/fixture.cpp)
    set_property(SOURCE platform/vita/benchmark/fixture.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita")
    set_property(SOURCE platform/vita/main_vita.cpp platform/vita/aurora/aurora_vita.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_BENCHMARK=1)
endif()

option(TWV_VITA_SCENE_PROBE "Read-only stage/player receipts after FIFO drain" OFF)
if(TWV_VITA_SCENE_PROBE)
    target_sources(twv_game PRIVATE platform/vita/runtime/scene_probe.cpp platform/vita/runtime/target_probe.cpp)
    set_property(SOURCE platform/vita/runtime/scene_probe.cpp platform/vita/runtime/target_probe.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita/runtime")
    set_property(SOURCE platform/vita/aurora/aurora_vita.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_SCENE_PROBE=1)
endif()

if(TWV_VITA_GFX_VGL)
    include(cmake/VitaGL.cmake)
    option(TWV_VITA_SAMPLER_CACHE "Skip identical object sampler updates with bounded exact texture-ID shadows" ON)
    option(TWV_VITA_UNIFORM_CACHE "Skip byte-identical vec4 uploads with bounded program-object shadows" ON)
    option(TWV_VITA_RASTER_CACHE "Skip unchanged draw raster state; invalidate at clear/frame/context boundaries" ON)
    option(TWV_VITA_SHADER_STAGE_CACHE "Share exact compiled Cg stages between independently linked programs" ON)
    option(TWV_VITA_SHADER_WARMUP "Prepare optional private Cg source bundle before gameplay; retain startup time receipts" ON)
    option(TWV_VITA_SHADER_SOURCE_CAPTURE "Record bounded generated Cg source for a private warmup bundle (diagnostic IO)" OFF)
    # gpu_dispatch.cpp exports twv_vgl_* and forwards to the start-up backend
    # (--vita-gpu); gpu_api.cpp is the vitaGL backend, gpu_common.cpp the shared receipts.
    target_sources(twv_vita_runtime PRIVATE platform/vita/gfx_vgl/gpu_api.cpp
        platform/vita/gfx_vgl/gpu_dispatch.cpp
        platform/vita/gfx_vgl/gpu_common.cpp
        platform/vita/gfx_vgl/frame_capture.cpp
        platform/vita/gfx_vgl/shader_preparation.cpp
        platform/vita/gfx_vgl/resident_geometry.cpp)
    set_property(SOURCE platform/vita/gfx_vgl/gpu_common.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_DRAW_TIMERS=$<BOOL:${TWV_VITA_DRAW_TIMERS}>)
    # G1: the direct GXM backend, selected at start-up with --vita-gpu=gxm (vitaGL stays default).
    option(TWV_VITA_GFX_GXM "Compile the start-up selectable direct GXM backend" ON)
    if(TWV_VITA_GFX_GXM)
        target_sources(twv_vita_runtime PRIVATE platform/vita/gfx_gxm/gxm_backend.cpp)
    endif()
    set_property(SOURCE platform/vita/gfx_vgl/gpu_dispatch.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_GFX_GXM=$<BOOL:${TWV_VITA_GFX_GXM}>)
    set_property(SOURCE platform/vita/gfx_vgl/shader_preparation.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_SHADER_WARMUP=$<BOOL:${TWV_VITA_SHADER_WARMUP}>
                            TWV_VITA_SHADER_SOURCE_CAPTURE=$<BOOL:${TWV_VITA_SHADER_SOURCE_CAPTURE}>)
    set_property(SOURCE platform/vita/gfx_vgl/gpu_api.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_SAMPLER_CACHE=$<BOOL:${TWV_VITA_SAMPLER_CACHE}>
                            TWV_VITA_UNIFORM_CACHE=$<BOOL:${TWV_VITA_UNIFORM_CACHE}>
                            TWV_VITA_RASTER_CACHE=$<BOOL:${TWV_VITA_RASTER_CACHE}>
                            TWV_VITA_SHADER_STAGE_CACHE=$<BOOL:${TWV_VITA_SHADER_STAGE_CACHE}>
                            TWV_VITA_DRAW_TIMERS=$<BOOL:${TWV_VITA_DRAW_TIMERS}>)
    target_link_libraries(twv_vita_runtime PUBLIC
        twv_xxhash twv_vitagl vitashark SceShaccCgExt mathneon zip z taihen_stub toloader
        SceShaccCg_stub SceGxm_stub SceDisplay_stub SceSysmodule_stub
        SceCommonDialog_stub SceAppMgr_stub SceAppUtil_stub SceKernelDmacMgr_stub SceCtrl_stub PNG::PNG)
endif()

# SDL3 is the image's prebuilt static library (short enums). It is linked now that
# twv_abi_audit has cleared the SDL structs our code reads (docs/ARCHITECTURE.md, "Enum ABI").
# twv_vita_runtime is in the group because patched game-side code (DuskAudioSystem)
# calls into it; outside, ld would scan it before those references exist.
set(TWV_GAME_LINK
        "$<LINK_GROUP:RESCAN,twv_vita_runtime,twv_game,twv_dusk,${JSYSTEM_LIBRARIES},twv_aurora,twv_borealis>"
        twv_aurora_mtx freeverb
        fmt::fmt absl::flat_hash_map twv_miniz twv_xxhash
        SDL3::SDL3-static
        # Whole archive: libstdc++'s gthread layer finds pthread through weak references
        # (__gthread_active_p). Linked normally they stay unresolved, std::thread throws
        # "Enable multithreading to use std::thread" (measured in Vita3K) and emutls
        # (thread_local) silently runs single-threaded.
        "-Wl,--whole-archive" pthread "-Wl,--no-whole-archive")

# newlib_gaps.c is an object of the executable, not an archive member, so it resolves
# references from libraries linked after the game group (fmt).
add_executable(twv_link_probe platform/vita/main_vita.cpp platform/vita/compat/newlib_gaps.c)
target_link_libraries(twv_link_probe PRIVATE ${TWV_GAME_LINK})
target_link_options(twv_link_probe PRIVATE -Wl,--unresolved-symbols=ignore-all)

set(TWV_STUBS_C "${CMAKE_BINARY_DIR}/generated/twv_stubs.c")
add_custom_command(
        OUTPUT "${TWV_STUBS_C}" "${CMAKE_BINARY_DIR}/link_report.txt"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/gen_stubs.py"
                --elf "$<TARGET_FILE:twv_link_probe>"
                --nm "${CMAKE_NM}" --readelf "${CMAKE_READELF}"
                --cxxfilt "$ENV{VITASDK}/bin/arm-vita-eabi-c++filt"
                --out "${TWV_STUBS_C}" --report "${CMAKE_BINARY_DIR}/link_report.txt"
        DEPENDS twv_link_probe "${CMAKE_SOURCE_DIR}/tools/gen_stubs.py"
        COMMENT "Generating trap stubs for unresolved symbols"
        VERBATIM)

add_executable(twilight_vita platform/vita/main_vita.cpp platform/vita/compat/newlib_gaps.c "${TWV_STUBS_C}")
target_link_libraries(twilight_vita PRIVATE ${TWV_GAME_LINK})
set_target_properties(twilight_vita PROPERTIES SUFFIX ".elf")

vita_create_self(eboot.bin twilight_vita.elf UNSAFE)
vita_create_vpk(twilight_vita.vpk ${TWV_TITLE_ID} eboot.bin
        VERSION ${TWV_VERSION}
        NAME "DuskOnVita (dev)")

# Audio output probe (platform/vita/probes/audio_probe.cpp): the SceAudioOut path
# without the game, same title ID so tools/vita3k_smoke.sh runs it as is.
add_executable(twv_audio_probe platform/vita/probes/audio_probe.cpp)
target_link_libraries(twv_audio_probe PRIVATE twv_vita_runtime "-Wl,--whole-archive" pthread "-Wl,--no-whole-archive")
set_source_files_properties(platform/vita/probes/audio_probe.cpp PROPERTIES COMPILE_OPTIONS -fshort-enums)
set_target_properties(twv_audio_probe PROPERTIES SUFFIX ".elf")
vita_create_self(twv_audio_probe.self twv_audio_probe.elf UNSAFE)
vita_create_vpk(twilight_vita_audio_probe.vpk ${TWV_TITLE_ID} twv_audio_probe.self
        VERSION ${TWV_VERSION}
        NAME "DuskOnVita audio probe")
