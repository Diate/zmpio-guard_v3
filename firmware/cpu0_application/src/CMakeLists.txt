# Copyright (C) 2023 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_SOURCE_DIR}/Hello_worldExample.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/UserConfig.cmake)
set(APP_NAME cpu0_application)
project(${APP_NAME})

enable_language(C ASM CXX)
find_package(common)
collect(PROJECT_LIB_DEPS xilstandalone)
collect(PROJECT_LIB_DEPS xil)
collect(PROJECT_LIB_DEPS xiltimer;lwip213)































































































collect(PROJECT_LIB_DEPS gcc)
collect(PROJECT_LIB_DEPS c)

collect (PROJECT_LIB_SOURCES cpu0_ipc.c)
collect (PROJECT_LIB_SOURCES cpu0_ipc_v3.c)
collect (PROJECT_LIB_SOURCES cpu0_irq_handler.c)
collect (PROJECT_LIB_SOURCES ethernet_test.c)
collect (PROJECT_LIB_SOURCES main.c)
collect (PROJECT_LIB_SOURCES net_lwip.c)
collect (PROJECT_LIB_SOURCES net_ping.c)
collect (PROJECT_LIB_SOURCES platform.c)
collect (PROJECT_LIB_SOURCES pl_doorbell.c)
collector_list (_sources PROJECT_LIB_SOURCES)
# Buoc 4 (docs/ROADMAP_DETAIL.md): shared CRC32 used by both CPU0 and CPU1 for
# ABI v3 (common/zmpio_abi_v3.h) lives in common/, not this directory. Vitis
# regenerates this file's collect()/collector_list() block on every app.build()
# and always resettles collector_list() right after its own recognized native
# sources, so a collect() call placed before it here does not survive rebuilds
# (confirmed: silently dropped from the linked ELF). Append to _sources
# directly instead, after it is defined -- the same pattern
# app_freertos/src/CMakeLists.txt already uses for this same file.
list(APPEND _sources "${CMAKE_CURRENT_SOURCE_DIR}/../../../common/zmpio_crc32.c")
foreach (source ${_sources})
    get_filename_component(ext ${source} EXT)
    list(APPEND src_ext ${ext})
endforeach()

find_project_type ("${src_ext}" PROJECT_TYPE)

if("${PROJECT_TYPE}" STREQUAL "c++")
collect(PROJECT_LIB_DEPS stdc++)
endif()
collector_list (_deps PROJECT_LIB_DEPS)
list (APPEND _deps ${USER_LINK_LIBRARIES})

if("${PROJECT_TYPE}" STREQUAL "c++")
string (REPLACE ";" ",-l" _deps "${_deps}")
endif()
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
    set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})
endif()
linker_gen("${CMAKE_CURRENT_SOURCE_DIR}/linker_files/")
string(APPEND CMAKE_C_FLAGS ${USER_COMPILE_OPTIONS})
string(APPEND CMAKE_CXX_FLAGS ${USER_COMPILE_OPTIONS})
string(APPEND CMAKE_C_LINK_FLAGS ${USER_LINK_OPTIONS})
string(APPEND CMAKE_CXX_LINK_FLAGS ${USER_LINK_OPTIONS})
set_source_files_properties(${_sources} OBJECT_DEPENDS "${CMAKE_LIBRARY_PATH}/*.a")
add_executable(${APP_NAME}.elf ${_sources})
set_target_properties(${APP_NAME}.elf PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/lscript.ld)
target_link_libraries(${APP_NAME}.elf -Wl,-T -Wl,\"${CMAKE_SOURCE_DIR}/lscript.ld\" -L\"${CMAKE_SOURCE_DIR}/\" -L\"${CMAKE_LIBRARY_PATH}/\" -L\"${USER_LINK_DIRECTORIES}/\" -Wl,--start-group,-l${_deps} -Wl,--end-group)
target_compile_definitions(${APP_NAME}.elf PUBLIC ${USER_COMPILE_DEFINITIONS})
target_include_directories(${APP_NAME}.elf BEFORE PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../common"
    ${USER_INCLUDE_DIRECTORIES})
print_elf_size(CMAKE_SIZE ${APP_NAME})
