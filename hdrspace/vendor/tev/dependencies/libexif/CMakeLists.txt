cmake_minimum_required(VERSION 3.15)

project(libexif)
set (VERSION "0.6.24")

option(BUILD_STATIC_LIBS "Build static library" ON)
option(BUILD_SHARED_LIBS "Build shared library" ON)


# Build configuration
################################################################################

set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# Platform specific build configuration
################################################################################

if(MSVC)
    message(STATUS "Building for windows")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()


# Source files
################################################################################

include_directories(.)

file(GLOB_RECURSE libexif_SRC libexif/*.c) # add headers and config.h ?
list(SORT libexif_SRC)


file(GLOB libexif_HEADERS libexif/exif-*.h)
list(SORT libexif_HEADERS)


# Build
################################################################################

# Build library
if (BUILD_SHARED_LIBS)
    message("** Building shared library")
    add_library(exif_shared SHARED ${libexif_SRC})
    target_link_libraries(exif_shared ${EXTRA_LIBS})
    target_include_directories(exif_shared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(exif_shared PROPERTIES OUTPUT_NAME exif)
endif (BUILD_SHARED_LIBS)

if (BUILD_STATIC_LIBS)
    message("** Building static library")
    add_library(exif_static STATIC ${libexif_SRC})
    target_link_libraries(exif_static ${EXTRA_LIBS})
    target_include_directories(exif_static INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(exif_static PROPERTIES OUTPUT_NAME exif)
endif (BUILD_STATIC_LIBS)


# Deployment
################################################################################

# pkg-config integration
set(PROJECT_VERSION "${VERSION}")
set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name")
set(EXEC_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX} CACHE PATH "Installation prefix for executables and object code libraries" FORCE)
set(BIN_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/bin CACHE PATH "Installation prefix for user executables" FORCE)
set(LIB_INSTALL_DIR ${EXEC_INSTALL_PREFIX}/lib${LIB_SUFFIX} CACHE PATH "Installation prefix for object code libraries" FORCE)
set(INCLUDE_INSTALL_DIR ${CMAKE_INSTALL_PREFIX}/include/libexif CACHE PATH "Installation prefix for header files" FORCE)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libexif.pc.cmake ${CMAKE_CURRENT_BINARY_DIR}/libexif.pc)

# Install the shared library and its header into the system (optional step, requires root credentials)
# Relative to $<INSTALL_PREFIX>
if (BUILD_SHARED_LIBS)
    install(TARGETS exif_shared DESTINATION lib)
endif (BUILD_SHARED_LIBS)
if (BUILD_STATIC_LIBS)
    install(TARGETS exif_static DESTINATION lib)
endif (BUILD_STATIC_LIBS)

install(FILES ${libexif_HEADERS} DESTINATION include/libexif/)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libexif.pc DESTINATION lib/pkgconfig/)
