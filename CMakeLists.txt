cmake_minimum_required(VERSION 3.15)
project(svg2pdf_publisher VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(RSVG REQUIRED librsvg-2.0)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

find_library(WOFF2_DEC_LIB NAMES woff2dec)
find_library(WOFF2_COMMON_LIB NAMES woff2common)
find_path(WOFF2_INCLUDE_DIR NAMES woff2/decode.h)

add_executable(publisher_convert main.cpp)

target_include_directories(publisher_convert PRIVATE 
    ${CAIRO_INCLUDE_DIRS} 
    ${RSVG_INCLUDE_DIRS} 
    ${FONTCONFIG_INCLUDE_DIRS}
    ${WOFF2_INCLUDE_DIR}
)

target_link_directories(publisher_convert PRIVATE
    ${CAIRO_LIBRARY_DIRS}
    ${RSVG_LIBRARY_DIRS}
    ${FONTCONFIG_LIBRARY_DIRS}
)

target_link_libraries(publisher_convert PRIVATE 
    ${CAIRO_LIBRARIES} 
    ${RSVG_LIBRARIES} 
    ${FONTCONFIG_LIBRARIES}
    ${WOFF2_DEC_LIB}
    ${WOFF2_COMMON_LIB}
)
