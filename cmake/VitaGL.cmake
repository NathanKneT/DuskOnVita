# Source-pinned native dependency, built offline in the same SDK image.
# The library's optional rotating splash adds a second GXM context and display
# thread before the game's first frame. Exclude it from the port's process.
find_program(TWV_GNU_MAKE make REQUIRED)
set(TWV_VGL_ROOT "${TWV_DEPS_DIR}/vitagl")
file(GLOB_RECURSE TWV_VGL_INPUTS CONFIGURE_DEPENDS
    "${TWV_VGL_ROOT}/source/*.c" "${TWV_VGL_ROOT}/source/*.cpp"
    "${TWV_VGL_ROOT}/source/*.h")
# fetch_deps.sh fingerprints and applies these to the verified source archive.
# Keep them explicit here so dependency/feature edits cannot reuse stale objects.
file(GLOB TWV_VGL_PATCHES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../patches/deps/vitagl/*.patch")
set(TWV_VGL_ARCHIVE "${TWV_VGL_ROOT}/libvitaGL.a")
add_custom_command(OUTPUT "${TWV_VGL_ARCHIVE}"
    # Make does not track changed feature flags in existing objects. Clean only
    # when this custom command is invalidated, so the cache option is reproducible.
    COMMAND "${TWV_GNU_MAKE}" clean
    COMMAND "${TWV_GNU_MAKE}" -j4 NO_SPLASHSCREEN=1 HAVE_SHADER_CACHE=1 HAVE_VERTEX_LAYOUT_CACHE=1 CIRCULAR_POOL_SPEEDHACK=0 LOG_ERRORS=1 VGL_GIT_HASH=7635e3c
    WORKING_DIRECTORY "${TWV_VGL_ROOT}"
    DEPENDS ${TWV_VGL_INPUTS} ${TWV_VGL_PATCHES}
        "${TWV_VGL_ROOT}/Makefile" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Building pinned vitaGL (no library splash, Cg shader cache, errors logged)"
    VERBATIM)
add_custom_target(twv_vitagl_build DEPENDS "${TWV_VGL_ARCHIVE}")
add_library(twv_vitagl STATIC IMPORTED GLOBAL)
set_target_properties(twv_vitagl PROPERTIES IMPORTED_LOCATION "${TWV_VGL_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${TWV_VGL_ROOT}/source")
add_dependencies(twv_vitagl twv_vitagl_build)
