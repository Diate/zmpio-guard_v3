# Buoc 7 host tests (docs/PLAN_BUOC_7.md SS4 7.2).
#
# Unlike libzmpio/zmpiod/zmpioctl these need no UIO, no board and no root, so
# they run on a plain development host.  They cover the two Buoc 7 changes
# whose failure mode is silent and therefore cannot be left for the board
# session to catch:
#
#   zmpio_v2_demux   -- R1: a request must not consume stream messages, and
#                       every kind of loss must be counted (drives the real
#                       zmpio_v2.c against a malloc'd copy of the shared
#                       window laid out exactly like the real one).
#   zmpio_capture_gen -- T7.2-5: what zmpiod writes must be a ZLOG container
#                       parse_zlog.py accepts.  The C target only generates;
#                       capture_roundtrip.py runs it and validates with the
#                       real parser.
#
# Run them with ctest from a native build:
#   cmake -B build -S software/linux && cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Portability, measured rather than assumed (2026-09-12): zmpio_v2_demux_test
# builds and passes under MinGW/Windows too.  zmpio_capture_gen does NOT --
# MinGW defaults to the ms_struct layout, so the packed ZLOG headers grow and
# the _Static_asserts in common/zmpio_zlog.h fire, and it also wants POSIX
# clock_gettime()/mkdir().  Run the capture round-trip on the PetaLinux VM (or
# any Linux host), which is where it is verified.

add_executable(zmpio_v2_demux_test zmpio_v2_demux_test.c)
target_include_directories(zmpio_v2_demux_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../libzmpio/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../libzmpio/src"
    "${ZMPIO_COMMON_DIR}"
)

add_executable(zmpio_capture_gen
    zmpio_capture_gen.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../zmpiod/zmpio_capture.c"
    "${ZMPIO_COMMON_DIR}/zmpio_crc32.c"
)
target_include_directories(zmpio_capture_gen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../zmpiod"
    "${ZMPIO_COMMON_DIR}"
)

if(NOT MSVC)
    target_compile_options(zmpio_v2_demux_test PRIVATE -Wall -Wextra)
    target_compile_options(zmpio_capture_gen PRIVATE -Wall -Wextra)
endif()

add_test(NAME zmpio_v2_demux COMMAND zmpio_v2_demux_test)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME zmpio_capture_roundtrip
             COMMAND ${Python3_EXECUTABLE}
                     "${CMAKE_CURRENT_SOURCE_DIR}/capture_roundtrip.py"
                     $<TARGET_FILE:zmpio_capture_gen>)
else()
    message(STATUS "python3 not found -- zmpio_capture_roundtrip test skipped")
endif()
