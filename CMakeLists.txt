cmake_minimum_required(VERSION 3.20)
project(MiniGit VERSION 0.1.0 LANGUAGES CXX)

option(MINIGIT_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)

add_library(minigit_options INTERFACE)
target_compile_features(minigit_options INTERFACE cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(minigit_options INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(MINIGIT_SANITIZERS)
        target_compile_options(minigit_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie)
        target_link_options(minigit_options INTERFACE -fsanitize=address,undefined -no-pie)
    endif()
endif()

add_library(minigit_core src/core/version.cpp)
target_include_directories(minigit_core PUBLIC include)
target_link_libraries(minigit_core PUBLIC minigit_options PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
set_target_properties(minigit_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(minigit src/cli/main.cpp)
target_link_libraries(minigit PRIVATE minigit_core)
set_target_properties(minigit PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_test(NAME cli_basics COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_basics.cmake)
endif()
