cmake_minimum_required(VERSION 3.15)
project(svg2pdf_publisher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Look up native vcpkg configurations directly (removes PkgConfig rules)
# 1. Print the primary system prefixes CMake searches
message(STATUS "DEBUG: CMAKE_SYSTEM_PREFIX_PATH = ${CMAKE_SYSTEM_PREFIX_PATH}")

# 2. Print the search paths specifically targeted for packages
message(STATUS "DEBUG: CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")

# 3. Print the vcpkg toolchain path to verify it is actually loaded
message(STATUS "DEBUG: CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")


# 1. Load CMake's native PkgConfig module
find_package(PkgConfig REQUIRED)

# 2. Use pkg-config to locate Cairo and create an IMPORTED target

pkg_check_modules(Fontconfig REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(Cairo REQUIRED IMPORTED_TARGET cairo)


pkg_check_modules(LIBRSVG librsvg-2.0 IMPORTED_TARGET REQUIRED)


# 2. Map standard static file searches for your WOFF2 decompression library
find_library(WOFF2_DEC_LIB NAMES woff2dec)
find_library(WOFF2_COMMON_LIB NAMES woff2common)

find_path(WOFF2_INCLUDE_DIR NAMES woff2/decode.h)


find_package(unofficial-brotli CONFIG REQUIRED)

add_executable(publisher_convert main.cpp)

# 3. Explicitly bind compiled library targets directly 
target_link_libraries(publisher_convert PRIVATE 
    PkgConfig::Cairo
    PkgConfig::LIBRSVG
    PkgConfig::Fontconfig
    ${WOFF2_DEC_LIB}
    ${WOFF2_COMMON_LIB}
    
    unofficial::brotli::brotlidec
    unofficial::brotli::brotlicommon
    
)

target_include_directories(publisher_convert PRIVATE ${WOFF2_INCLUDE_DIR})


# svg_inspector

add_executable(svg_inspector WIN32 svg_inspector_main.cpp)

target_link_libraries(svg_inspector PRIVATE 
    PkgConfig::Cairo
    PkgConfig::LIBRSVG
    PkgConfig::Fontconfig
    
    ${WOFF2_DEC_LIB}
    ${WOFF2_COMMON_LIB}
    
    unofficial::brotli::brotlidec
    unofficial::brotli::brotlicommon

    
    user32 
    gdi32 
    comdlg32
)
