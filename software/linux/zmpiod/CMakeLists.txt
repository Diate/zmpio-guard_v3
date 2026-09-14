# zmpiod -- Buoc 6.4. Cross-build the same way as libzmpio
# (software/linux/libzmpio/CMakeLists.txt); normally built via the
# parent software/linux/CMakeLists.txt so it picks up the zmpio target
# without a separate configure step.

# zmpio_capture.c / zmpio_stats.c are Buoc 7.2/7.4 additions: the ZLOG
# capture writer and the latency/jitter histograms.  They live here rather
# than in libzmpio because they are daemon policy (where files go, how they
# rotate, which percentiles are reported), not transport.
add_executable(zmpiod
    zmpiod.c
    zmpio_capture.c
    zmpio_stats.c
)
target_link_libraries(zmpiod PRIVATE zmpio)
target_include_directories(zmpiod PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(NOT MSVC)
    target_compile_options(zmpiod PRIVATE -Wall -Wextra)
endif()
# _GNU_SOURCE already #defined at the top of zmpiod.c -- see the comment in
# ../libzmpio/CMakeLists.txt for why it is not also set here.

include(GNUInstallDirs)
install(TARGETS zmpiod RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
