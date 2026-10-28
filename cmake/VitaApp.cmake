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
        platform/vita/runtime/fast_shadow_collect.cpp
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
        SceAudio_stub SceDisplay_stub SceCtrl_stub)
set_source_files_properties(platform/vita/main_vita.cpp platform/vita/compat/newlib_gaps.c PROPERTIES COMPILE_OPTIONS -fshort-enums)
# patches/dusklight/0016: game.buttonPrompts=playstation draws its button glyphs here (no artwork).
target_sources(twv_vita_runtime PRIVATE platform/vita/ui/ps_glyphs.cpp)
# Start-up progress and error screens (ui/startup_screen.hpp): CPU-drawn, shown with sceDisplay.
target_sources(twv_vita_runtime PRIVATE platform/vita/ui/startup_screen.cpp)
# The shader warm-up bundle inside the VPK (app0:shader_warmup.bin), read when the data folder has
# none. Off by default: the bundle is derived from the game's materials and whether to distribute it
# is the project owner's decision. It is never part of the repository; name a local file to package.
option(TWV_VITA_APP_WARMUP_BUNDLE "Package a shader warm-up bundle as app0:shader_warmup.bin and read it when ux0 has none" OFF)
set(TWV_VITA_APP_WARMUP_BUNDLE_FILE "" CACHE FILEPATH "Local shader_warmup.bin to package with TWV_VITA_APP_WARMUP_BUNDLE")
set(TWV_VPK_EXTRA_FILES)
if(TWV_VITA_APP_WARMUP_BUNDLE)
    if(NOT EXISTS "${TWV_VITA_APP_WARMUP_BUNDLE_FILE}")
        message(FATAL_ERROR "TWV_VITA_APP_WARMUP_BUNDLE needs TWV_VITA_APP_WARMUP_BUNDLE_FILE (a local shader_warmup.bin)")
    endif()
    set(TWV_VPK_EXTRA_FILES FILE "${TWV_VITA_APP_WARMUP_BUNDLE_FILE}" shader_warmup.bin)
endif()

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
# platform/vita/touch.cpp: front touch to the menu pointer (patches/dusklight/0017).
add_library(twv_dusk STATIC ${DUSK_FILES} ${TWV_DUSK_AUDIO_FILES} platform/vita/dusk/mods_gfx_vita.cpp
        platform/vita/touch.cpp)
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
else()
    # Test-only scripted pad input (--vita-input-script), on the same pad hook as the fixture.
    target_sources(twv_game PRIVATE platform/vita/runtime/input_script.cpp)
    set_property(SOURCE platform/vita/runtime/input_script.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita/runtime")
    # main_vita.cpp and the pad hook refer to it weakly, which never pulls an archive member in.
    target_link_options(twv_game INTERFACE -Wl,-u,twv_vita_input_script_load)
endif()

option(TWV_VITA_SCENE_PROBE "Read-only stage/player receipts after FIFO drain" OFF)
if(TWV_VITA_SCENE_PROBE)
    target_sources(twv_game PRIVATE platform/vita/runtime/scene_probe.cpp platform/vita/runtime/target_probe.cpp)
    set_property(SOURCE platform/vita/runtime/scene_probe.cpp platform/vita/runtime/target_probe.cpp APPEND PROPERTY
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita/runtime")
    set_property(SOURCE platform/vita/aurora/aurora_vita.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS TWV_VITA_SCENE_PROBE=1)
endif()

# Main-thread actor profile (measurement only, full receipts only): per-profile execute/draw time,
# culling and camera distance, J3DModel parts (patches/dusklight/0020 calls it, so always built).
target_sources(twv_game PRIVATE platform/vita/runtime/actor_profile.cpp)
set_property(SOURCE platform/vita/runtime/actor_profile.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/platform/vita/runtime")
target_link_options(twv_game INTERFACE -Wl,-u,twv_actprof_phase
    -Wl,--wrap=_Z13fpcEx_ExecuteP18base_process_class -Wl,--wrap=_Z13fpcDw_ExecuteP18base_process_class
    -Wl,--wrap=_Z19fopAcM_cullingCheckPK10fopAc_ac_c -Wl,--wrap=_Z17fpcM_DrawIteraterPFiPvS_E
    -Wl,--wrap=_ZNK13J3DDrawBuffer4drawEv)

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
                            TWV_VITA_SHADER_SOURCE_CAPTURE=$<BOOL:${TWV_VITA_SHADER_SOURCE_CAPTURE}>
                            TWV_VITA_APP_WARMUP_BUNDLE=$<BOOL:${TWV_VITA_APP_WARMUP_BUNDLE}>)
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

# TWV_OPT=*LTO and ld --wrap: GCC LTO binds a call straight to a definition that is in
# the same LTO unit, so ld never sees the undefined reference it would wrap (measured with
# arm-vita-eabi GCC 15.2 / binutils 2.46: caller and callee both IR -> the wrapper is never
# called; either one a regular object -> wrapped). The profile hooks, --vita-pipeline-fast,
# the build_uniform replacement and the decode probe all depend on wraps, so every
# translation unit that defines a wrapped symbol stays a regular object (same call sites
# as the -O2 build). Aurora's io.cpp too: its -Werror=frame-larger-than guard (32 KiB worker
# stacks) only runs on a regular compile. Keep this list in step with the --wrap options.
if (TWV_LTO)
    set_property(SOURCE
            ${DUSK_ROOT}/src/f_ap/f_ap_game.cpp                   # fapGm_Execute
            ${DUSK_ROOT}/src/m_Do/m_Do_audio.cpp                  # mDoAud_Execute
            ${DUSK_ROOT}/src/SSystem/SComponent/c_API_graphic.cpp # cAPIGph_Painter
            ${DUSK_ROOT}/src/f_pc/f_pc_executor.cpp               # fpcEx_Handler
            ${DUSK_ROOT}/src/f_pc/f_pc_draw.cpp                   # fpcDw_Handler
            ${DUSK_ROOT}/src/d/d_particle.cpp                     # dPa_control_c::calc3D
            ${DUSK_ROOT}/src/d/d_bg_s.cpp                         # cBgS::ShdwDraw, GroundCross, LineCross
            ${DUSK_ROOT}/src/d/d_bg_s_acch.cpp                    # dBgS_Acch::CrrPos
            ${DUSK_ROOT}/src/d/d_cc_s.cpp                         # dCcS::Move
            ${DUSK_ROOT}/src/d/d_demo.cpp                         # dDemo_c::update
            ${DUSK_ROOT}/src/dusk/stubs.cpp                       # VIWaitForRetrace
            ${AURORA_ROOT}/lib/gx/command_processor.cpp           # fifo::process
            ${AURORA_ROOT}/lib/gx/regs.cpp                        # fifo::handle_bp/cp/xf, copy_xf_data
            ${AURORA_ROOT}/lib/gx/shader_info.cpp                 # build_uniform
            ${AURORA_ROOT}/lib/gx/texture.cpp                     # texture::current_bind_generation
            platform/vita/gfx_vgl/backend.cpp                     # populate_pipeline_config, pipeline_ref,
                                                                  # push_verts/indices, push_draw_command
            platform/vita/gfx_vgl/vertex.cpp                      # twv_gfx_static_dl, twv_gfx_array_base
            ${AURORA_ROOT}/lib/io.cpp
            APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
endif ()

vita_create_self(eboot.bin twilight_vita.elf UNSAFE)
vita_create_vpk(twilight_vita.vpk ${TWV_TITLE_ID} eboot.bin
        VERSION ${TWV_VERSION}
        NAME "DuskOnVita (dev)"
        ${TWV_VPK_EXTRA_FILES})

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
