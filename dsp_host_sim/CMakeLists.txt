cmake_minimum_required(VERSION 3.20)
project(zmpio_step1_dsp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(step1_dsp STATIC
    fpga/dsp_core/src/fir3_axis.cpp
    fpga/dsp_core/src/fft128_real.cpp
    fpga/dsp_core/src/feature_extract.cpp
    fpga/dsp_core/src/dsp_core.cpp
)
target_include_directories(step1_dsp PUBLIC fpga/dsp_core/src)

if(MSVC)
    target_compile_options(step1_dsp PRIVATE /W4 /WX)
else()
    target_compile_options(step1_dsp PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(step1_tests fpga/dsp_core/tb/tb_dsp_core.cpp)
target_link_libraries(step1_tests PRIVATE step1_dsp)

add_executable(step1_golden fpga/dsp_core/tb/gen_golden.cpp)
target_link_libraries(step1_golden PRIVATE step1_dsp)

enable_testing()
add_test(NAME step1_dsp_regression COMMAND step1_tests)

