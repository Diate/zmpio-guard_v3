# Buoc 6 (docs/PLAN_BUOC_6.md, LIM-LNX-012): top-level build for the
# PetaLinux-side ABI v2/v3 client stack. Cross-build for the ARM target,
# e.g.:
#   cmake -B build -S software/linux \
#         -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
#         -DCMAKE_INSTALL_PREFIX=/usr
#   cmake --build build
#   cmake --install build --prefix <staging_dir>
#
# zmpiod must be the single reader of the ABI v2 TX ring (REQ-STR-002): the
# ring is single-producer/single-consumer, so a second reader silently steals
# feature-stream records.  parse_zlog.py needs no build step;
# tests/capture_roundtrip.py calls it directly.

cmake_minimum_required(VERSION 3.16)
project(zmpio_linux LANGUAGES C)

get_filename_component(ZMPIO_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(ZMPIO_COMMON_DIR "${ZMPIO_REPO_ROOT}/common")

add_subdirectory(libzmpio)
add_subdirectory(zmpiod)
add_subdirectory(zmpioctl)

# Buoc 7 host tests.  They need no target hardware, so they are built
# whenever this project is configured (including a cross-build, where they
# simply are not runnable) and registered with ctest for a native build.
enable_testing()
add_subdirectory(tests)
