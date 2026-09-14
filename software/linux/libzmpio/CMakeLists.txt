# libzmpio -- Buoc 6.3 (docs/PLAN_BUOC_6.md SS4 6.3, LIM-LNX-012).
#
# Builds for the PetaLinux ARM target, not the host -- unlike
# dsp_host_sim/CMakeLists.txt (a host-side simulation), there is nothing
# useful to build or test on Windows/x86 here: every source file talks to
# /sys/class/uio and mmap()s device memory that only exists on the board.
#
# Cross-build:
#   cmake -B build -S software/linux/libzmpio \
#         -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
#         -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build
#
# Native build directly on a PetaLinux target once GAP-006/T6.0-4's
# ssh/scp lands (drop -DCMAKE_C_COMPILER):
#   cmake -B build -S software/linux/libzmpio
#   cmake --build build
#
# Not yet wired into a PetaLinux Yocto recipe (bitbake .bb) -- that is the
# remaining half of LIM-LNX-012, tracked for whoever picks up Buoc 6.4's
# systemd/rootfs packaging.

cmake_minimum_required(VERSION 3.16)
project(zmpio_libzmpio LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_filename_component(ZMPIO_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(ZMPIO_COMMON_DIR "${ZMPIO_REPO_ROOT}/common")

add_library(zmpio STATIC
    src/libzmpio.c
    src/zmpio_uio.c
    src/zmpio_v2.c
    src/zmpio_v3.c
    "${ZMPIO_COMMON_DIR}/zmpio_crc32.c"
)

# common/ is PUBLIC, not PRIVATE: include/libzmpio.h itself #includes
# zmpio_abi_v3.h and zmpio_protocol.h, so any consumer that links against
# this target (e.g. zmpiod/zmpioctl, Buoc 6.4) needs common/ on its include
# path too, transitively, the same way it would if this were add_subdirectory'd
# from a parent CMakeLists that also builds those.
target_include_directories(zmpio
    PUBLIC
        include
        "${ZMPIO_COMMON_DIR}"
    PRIVATE
        src
)

if(NOT MSVC)
    target_compile_options(zmpio PRIVATE -Wall -Wextra)
endif()

# NOT set here: every .c file already #defines _GNU_SOURCE itself before any
# system header (confirmed by a real build on the PetaLinux VM, 2026-09-08 --
# adding it here too just produces a harmless but noisy "_GNU_SOURCE
# redefined" warning on every file).

include(GNUInstallDirs)
install(TARGETS zmpio ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/libzmpio.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
