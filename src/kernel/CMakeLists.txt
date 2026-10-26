cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_script_anchor.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_vsh_interp.c
    nv2a_combiner.c
    nv2a_vsh_cpu.c
    frontend_lifted.c
)

# XBOXRECOMP_FRONTEND=direct: the program runs guest code itself and defines
# xbox/xbox_frontend.h and the symbols of the files in lifted-only.txt (one
# file name per line), which are left out here. Programs that build these
# sources without CMake read the same list.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/lifted-only.txt KERNEL_LIFTED_ONLY)
if(XBOXRECOMP_FRONTEND STREQUAL "direct")
    list(REMOVE_ITEM KERNEL_SOURCES ${KERNEL_LIFTED_ONLY})
endif()

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
    ${XBOXRECOMP_SRC_DIR}/../include
)

if(WIN32 AND XBOXRECOMP_UWP)
    # The program's WindowsApp.lib supplies kernel32's names; bcrypt is in
    # it too (BCryptGenRandom), and the library is on the app allowlist.
    target_link_libraries(xbox_kernel PUBLIC platform bcrypt)
elseif(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # POSIX hosts: platform layer (host primitives). kernel_crypto.c carries its
    # own SHA-1, RC4 and HMAC, so no crypto library is linked (it used to link
    # OpenSSL without calling it, which a machine without one could not build).
    target_link_libraries(xbox_kernel PUBLIC platform)
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
