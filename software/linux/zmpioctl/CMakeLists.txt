# zmpioctl -- Buoc 6.4. No dependency on libzmpio (it only ever talks to
# zmpiod's Unix socket, never touches UIO/shared DDR directly), so this
# target links nothing but the shared protocol header.

add_executable(zmpioctl zmpioctl.c)
target_include_directories(zmpioctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

if(NOT MSVC)
    target_compile_options(zmpioctl PRIVATE -Wall -Wextra)
endif()
# _GNU_SOURCE already #defined at the top of zmpioctl.c -- see the comment
# in ../libzmpio/CMakeLists.txt for why it is not also set here.

include(GNUInstallDirs)
install(TARGETS zmpioctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
