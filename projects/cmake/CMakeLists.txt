cmake_minimum_required(VERSION 3.11)
project (Wren
    VERSION 0.4.0
    DESCRIPTION "Wren: A small, fast, class-based concurrent scripting language"
    HOMEPAGE_URL "https://github.com/wren-lang/wren"
    LANGUAGES C CXX
)

# Options
option(WREN_META "Wren Meta Class" ON)
option(WREN_RANDOM "Wren Random Class" ON)
option(WREN_BUILD_STATIC "Wren Static Library" ON)
option(WREN_BUILD_SHARED "Wren Shared Library" ON)
option(WREN_BUILD_TEST "Build Wren Tests" OFF)
option(WREN_DEBUGGER "Wren Debug Adapter Protocol debugger" ON)

# Build tests by default if compiling in the root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WREN_BUILD_TEST TRUE)
endif()

# Source Files
set(WREN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
file(GLOB WREN_SRC_VM_FILES ${WREN_SRC}/vm/*.cpp)
file(GLOB WREN_SRC_OPTIONAL_FILES ${WREN_SRC}/optional/*.cpp)
set(WREN_SRC_FILES ${WREN_SRC_VM_FILES} ${WREN_SRC_OPTIONAL_FILES})
set(WREN_INCLUDE_DIRS
    ${WREN_SRC}/include
    ${WREN_SRC}/optional
    ${WREN_SRC}/vm
)

# Defines
set(WREN_DEFINES)
if (WREN_META)
    list(APPEND WREN_DEFINES WREN_OPT_META)
endif()
if (WREN_RANDOM)
    list(APPEND WREN_DEFINES WREN_OPT_RANDOM)
endif()

# C++20 standard settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Wren Libraries
if (WREN_BUILD_STATIC)
    add_library(wren STATIC ${WREN_SRC_FILES})
    target_include_directories(wren PUBLIC ${WREN_INCLUDE_DIRS})
    target_compile_definitions(wren PUBLIC ${WREN_DEFINES})
endif()
if (WREN_BUILD_SHARED)
    add_library(wren_shared SHARED ${WREN_SRC_FILES})
    target_include_directories(wren_shared PUBLIC ${WREN_INCLUDE_DIRS})
    target_compile_definitions(wren_shared PUBLIC ${WREN_DEFINES})
endif()

# DAP debugger library (optional; hosts link this to make their scripts
# debuggable from VS Code or any other Debug Adapter Protocol client)
if (WREN_DEBUGGER AND WREN_BUILD_STATIC)
    file(GLOB WREN_SRC_DEBUGGER_FILES ${WREN_SRC}/debugger/*.cpp)
    add_library(wren_debugger STATIC ${WREN_SRC_DEBUGGER_FILES})
    target_include_directories(wren_debugger PUBLIC
        ${WREN_SRC}/debugger
        ${WREN_SRC}/include
        ${WREN_SRC}/vm
    )
    target_compile_definitions(wren_debugger PUBLIC ${WREN_DEFINES} WREN_ENABLE_DEBUGGER)
    target_compile_features(wren_debugger PUBLIC cxx_std_20)
    target_link_libraries(wren_debugger PUBLIC wren)

    if (WIN32)
        # Winsock, needed by the DAP socket layer on Windows.
        target_link_libraries(wren_debugger PUBLIC ws2_32)
    endif()

    if (WREN_BUILD_TEST)
        # A tiny standalone host so the debugger can be exercised without an
        # embedding application.
        add_executable(wren_debug_example ${WREN_SRC}/../example/debugger/main.cpp)
        target_link_libraries(wren_debug_example PUBLIC wren_debugger)
    endif()
endif()

# Testing
if(WREN_BUILD_TEST AND WREN_BUILD_STATIC)
    include(CTest)
    enable_testing()
    set(CTEST_CUSTOM_TESTS_IGNORE pkg-config--static)

    set(WREN_TEST ${WREN_SRC}/../test)
    file(GLOB WREN_TEST_FILES ${WREN_TEST}/api/*.cpp)
    list(APPEND WREN_TEST_FILES
        ${WREN_TEST}/main.cpp
        ${WREN_TEST}/test.cpp
    )

    add_executable(wren_test ${WREN_TEST_FILES})
    target_link_libraries(wren_test PUBLIC wren m)

    add_test(NAME wren_test COMMAND wren_test ${WREN_TEST}/core/system/print.wren)
    
    # Standalone C++20 bindings test
    add_executable(cpp20_bindings_test ${WREN_TEST}/cpp20_bindings_test.cpp)
    target_link_libraries(cpp20_bindings_test PUBLIC wren m)
    if (WREN_DEBUGGER)
        # Enables the debugger section of wren.hpp for the test.
        target_link_libraries(cpp20_bindings_test PUBLIC wren_debugger)
    endif()
    
    add_test(NAME cpp20_bindings_test COMMAND cpp20_bindings_test)
  endif()
