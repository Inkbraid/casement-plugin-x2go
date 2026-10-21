cmake_minimum_required(VERSION 3.24)
project(casement_x2go VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    # vcpkg's x64-windows-static libraries use the static C runtime; so does
    # the plugin, which keeps it one self-contained exe.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(cmake/casement.cmake)
enable_testing()

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(libssh2 CONFIG REQUIRED)

# The protocol pieces and tables, shared by the plugin and the unit tests.
add_library(x2go_core STATIC
    src/x2go/clipboard.cpp
    src/x2go/cursor.cpp
    src/x2go/frames.cpp
    src/x2go/image.cpp
    src/x2go/kdrive.cpp
    src/x2go/keyboard.cpp
    src/x2go/known_hosts.cpp
    src/x2go/options.cpp
    src/x2go/server.cpp)
target_include_directories(x2go_core PUBLIC src)
target_link_libraries(x2go_core PUBLIC casement ZLIB::ZLIB OpenSSL::Crypto windowscodecs ole32 uuid
    ws2_32 crypt32 bcrypt user32 advapi32)

casement_plugin_executable(casement-x2go src/main.cpp src/x2go/session.cpp src/x2go/ssh.cpp)
target_link_libraries(casement-x2go PRIVATE x2go_core libssh2::libssh2)
target_compile_definitions(casement-x2go PRIVATE X2GO_PLUGIN_VERSION="${PROJECT_VERSION}")

casement_test(unit_tests unit
    tests/unit/clipboard_tests.cpp
    tests/unit/frames_tests.cpp
    tests/unit/kdrive_tests.cpp
    tests/unit/keyboard_tests.cpp
    tests/unit/known_hosts_tests.cpp
    tests/unit/main.cpp
    tests/unit/server_tests.cpp)
target_link_libraries(unit_tests PRIVATE x2go_core)
