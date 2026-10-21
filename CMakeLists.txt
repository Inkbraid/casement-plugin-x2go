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
