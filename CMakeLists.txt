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

add_library(minigit_core
    src/core/version.cpp
    src/core/bytes.cpp
    src/core/fs.cpp
    src/core/object_id.cpp
    src/core/objects.cpp
    src/core/object_store.cpp
    src/core/repository.cpp
    src/core/transaction.cpp
    src/core/reflog.cpp
    src/core/branches.cpp
    src/core/switch.cpp
    src/core/restore.cpp
    src/core/index.cpp
    src/core/worktree.cpp
    src/core/worktree_update.cpp
    src/core/refs.cpp
    src/core/snapshot.cpp
    src/core/history.cpp
    src/core/format.cpp
    src/core/diff.cpp
    src/core/inspection.cpp
    src/core/status.cpp
    src/core/dag.cpp
    src/core/tags.cpp)
target_include_directories(minigit_core PUBLIC include)
target_link_libraries(minigit_core PUBLIC minigit_options PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
set_target_properties(minigit_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(minigit src/cli/main.cpp)
target_link_libraries(minigit PRIVATE minigit_core)
set_target_properties(minigit PROPERTIES CXX_EXTENSIONS OFF)

# Optional learning experiment: uses the public transaction injection API.
add_executable(minigit_transaction_lab EXCLUDE_FROM_ALL examples/experiments/transaction.cpp)
target_link_libraries(minigit_transaction_lab PRIVATE minigit_core)
set_target_properties(minigit_transaction_lab PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)
if(BUILD_TESTING)
    find_package(Catch2 3 REQUIRED)
    add_executable(minigit_tests
        tests/unit/test_objects.cpp
        tests/unit/test_transaction.cpp
        tests/unit/test_reflog.cpp
        tests/unit/test_branches.cpp
        tests/unit/test_switch.cpp
        tests/unit/test_restore.cpp
        tests/unit/test_repository.cpp
        tests/unit/test_staging.cpp
        tests/unit/test_worktree_update.cpp
        tests/unit/test_history.cpp
        tests/unit/test_status.cpp
        tests/unit/test_dag.cpp
        tests/unit/test_tags.cpp
        tests/unit/test_diff.cpp
        tests/unit/test_inspection.cpp)
    target_include_directories(minigit_tests PRIVATE tests/support)
    # Transaction crash scenarios also exercise recovery through the real CLI.
    add_dependencies(minigit_tests minigit)
    target_link_libraries(minigit_tests PRIVATE minigit_core Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(minigit_tests TEST_PREFIX "unit.")
    add_test(NAME cli_basics COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/cli_basics.cmake)
    add_test(NAME objects_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/objects_cli.cmake)
    add_test(NAME staging_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/staging_cli.cmake)
    add_test(NAME commits_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/commits_cli.cmake)
    add_test(NAME status_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/status_cli.cmake)
    add_test(NAME log_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/log_cli.cmake)
    add_test(NAME tags_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tags_cli.cmake)
    add_test(NAME diff_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/diff_cli.cmake)
    add_test(NAME recovery_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/recovery_cli.cmake)
    add_test(NAME show_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/show_cli.cmake)
    add_test(NAME reflog_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/reflog_cli.cmake)
    add_test(NAME branches_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/branches_cli.cmake)
    add_test(NAME switch_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/switch_cli.cmake)
    add_test(NAME restore_cli COMMAND ${CMAKE_COMMAND}
        -DMINIGIT=$<TARGET_FILE:minigit> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/restore_cli.cmake)
endif()
