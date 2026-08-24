cmake_minimum_required(VERSION 3.15)
project(svg2pdf_publisher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Look up native vcpkg configurations directly (removes PkgConfig rules)
find_package(unofficial-cairo CONFIG REQUIRED)
find_package(unofficial-librsvg CONFIG REQUIRED)
find_package(Fontconfig CONFIG REQUIRED)

# 2. Map standard static file searches for your WOFF2 decompression library
find_library(WOFF2_DEC_LIB NAMES woff2dec)
find_library(WOFF2_COMMON_LIB NAMES woff2common)
find_path(WOFF2_INCLUDE_DIR NAMES woff2/decode.h)

add_executable(publisher_convert main.cpp)

# 3. Explicitly bind compiled library targets directly 
target_link_libraries(publisher_convert PRIVATE 
    unofficial::cairo::cairo
    unofficial::librsvg::librsvg
    Fontconfig::Fontconfig
    ${WOFF2_DEC_LIB}
    ${WOFF2_COMMON_LIB}
)

target_include_directories(publisher_convert PRIVATE ${WOFF2_INCLUDE_DIR})
