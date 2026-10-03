cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()
project(mergehdrcore)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MERGEHDR_ENABLE_OPENEXR "Enable OpenEXR/Imath output support" ON)
option(MERGEHDR_ENABLE_OPENMP "Enable OpenMP acceleration when available" ON)
set(MERGEHDR_BOOST_MIN_VERSION "1.90.0" CACHE STRING "Minimum Boost version for config package lookup")
set(MERGEHDR_HOMEBREW_PREFIX "$ENV{HOMEBREW_PREFIX}" CACHE PATH "Optional Homebrew prefix for dependency hints")
set(MERGEHDR_MEX_DIR "" CACHE PATH "Path to matlabPyrTools/MEX directory containing convolve.c and edges.c")

if(NOT MERGEHDR_HOMEBREW_PREFIX AND EXISTS "/opt/homebrew")
  set(MERGEHDR_HOMEBREW_PREFIX "/opt/homebrew" CACHE PATH "Optional Homebrew prefix for dependency hints" FORCE)
endif()

if(NOT MERGEHDR_MEX_DIR)
  set(_mergehdr_mex_candidates
    "$ENV{HDRVDP_MEX_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../hdrspace/vendor/hdrvdp-mex"
    "${CMAKE_CURRENT_SOURCE_DIR}/../HDRVDP_view_analysis/hdrvdp3/matlabPyrTools/MEX"
  )
  foreach(candidate IN LISTS _mergehdr_mex_candidates)
    if(candidate AND EXISTS "${candidate}/convolve.c" AND EXISTS "${candidate}/edges.c")
      set(MERGEHDR_MEX_DIR "${candidate}" CACHE PATH "Path to matlabPyrTools/MEX directory containing convolve.c and edges.c" FORCE)
      break()
    endif()
  endforeach()
endif()

if(NOT MERGEHDR_MEX_DIR OR NOT EXISTS "${MERGEHDR_MEX_DIR}/convolve.c" OR NOT EXISTS "${MERGEHDR_MEX_DIR}/edges.c")
  message(FATAL_ERROR "MERGEHDR_MEX_DIR must point to the matlabPyrTools/MEX directory containing convolve.c and edges.c.")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/misc/")

# Keep build-machine paths (source folders, home directory) out of the binaries:
# __FILE__ and debug info refer to /src/... instead.
get_filename_component(_mergehdr_source_real "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_mergehdr_mex_real "${MERGEHDR_MEX_DIR}" REALPATH)
foreach(_mergehdr_prefix IN ITEMS
    "${CMAKE_CURRENT_SOURCE_DIR}=/src/mergehdr"
    "${_mergehdr_source_real}=/src/mergehdr"
    "${MERGEHDR_MEX_DIR}=/src/hdrvdp-mex"
    "${_mergehdr_mex_real}=/src/hdrvdp-mex")
  add_compile_options("-ffile-prefix-map=${_mergehdr_prefix}" "-fmacro-prefix-map=${_mergehdr_prefix}")
endforeach()
if(DEFINED ENV{HOME})
  add_compile_options("-ffile-prefix-map=$ENV{HOME}=/home" "-fmacro-prefix-map=$ENV{HOME}=/home")
endif()

find_package(PkgConfig REQUIRED)
find_package(JPEG REQUIRED)
if(MERGEHDR_ENABLE_OPENEXR)
  find_package(OpenEXR CONFIG REQUIRED)
  find_package(Imath CONFIG REQUIRED)
endif()
find_package(exiv2 CONFIG REQUIRED)
if(MERGEHDR_ENABLE_OPENMP)
  find_package(OpenMP)
endif()
find_package(Eigen3 CONFIG REQUIRED)
find_package(boost_headers ${MERGEHDR_BOOST_MIN_VERSION} CONFIG REQUIRED)
find_package(boost_filesystem ${MERGEHDR_BOOST_MIN_VERSION} CONFIG REQUIRED)
find_package(boost_program_options ${MERGEHDR_BOOST_MIN_VERSION} CONFIG REQUIRED)
pkg_check_modules(LIBRAW REQUIRED libraw)

set(_mergehdr_libomp_include_hints)
set(_mergehdr_libomp_lib_hints)
if(MERGEHDR_HOMEBREW_PREFIX)
  list(APPEND _mergehdr_libomp_include_hints "${MERGEHDR_HOMEBREW_PREFIX}/opt/libomp/include")
  list(APPEND _mergehdr_libomp_lib_hints "${MERGEHDR_HOMEBREW_PREFIX}/opt/libomp/lib")
endif()
find_path(LIBOMP_INCLUDE_DIR omp.h HINTS ${_mergehdr_libomp_include_hints})
find_library(LIBOMP_LIBRARY omp HINTS ${_mergehdr_libomp_lib_hints})

if("${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")
  find_library(CORESERVICES_LIBRARY CoreServices)
  find_library(ACCELERATE_FRAMEWORK Accelerate)
endif()
set(_mergehdr_fftw_include_hints /usr/local/include)
set(_mergehdr_fftw_lib_hints /usr/local/lib)
if(MERGEHDR_HOMEBREW_PREFIX)
  list(PREPEND _mergehdr_fftw_include_hints "${MERGEHDR_HOMEBREW_PREFIX}/include")
  list(PREPEND _mergehdr_fftw_lib_hints "${MERGEHDR_HOMEBREW_PREFIX}/lib")
endif()
find_path(FFTW3_INCLUDE_DIR fftw3.h HINTS ${_mergehdr_fftw_include_hints})
find_library(FFTW3_LIBRARY fftw3 HINTS ${_mergehdr_fftw_lib_hints})
find_library(FFTW3_OMP_LIBRARY fftw3_omp HINTS ${_mergehdr_fftw_lib_hints})

include_directories(${EXIV2_INCLUDE_DIR})
include_directories(${JPEG_INCLUDE_DIR})
include_directories(${LIBRAW_INCLUDE_DIRS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/dht_include)
include_directories(${MERGEHDR_MEX_DIR})
if(FFTW3_INCLUDE_DIR)
	include_directories(${FFTW3_INCLUDE_DIR})
endif()
if(LIBOMP_INCLUDE_DIR)
	include_directories(${LIBOMP_INCLUDE_DIR})
endif()

# Compile with modern C++ features required by the current helper sources
set(CMAKE_CXX_FLAGS "-std=c++17 -Wall -Wno-deprecated-declarations -Wno-deprecated-register -Wno-unused-local-typedefs ${CMAKE_CXX_FLAGS}")

add_definitions(-D_USE_MATH_DEFINES -D_UNICODE -DUNICODE)

add_executable(mergehdrcore input.cpp output.cpp main.cpp hdr.cpp fitexp.cpp resample.cpp misc.cpp dht_port.cpp dht_demosaic_linearhdr.cpp camera_detect.cpp)

add_executable(mergehdr
  mergehdr_cli.cpp
  camera_detect.cpp
  colorcalibrate.cpp
  chartcells.cpp
  glareeval.cpp
  hdrimage.cpp
  shadowband_native.cpp
  "${MERGEHDR_MEX_DIR}/convolve.c"
  "${MERGEHDR_MEX_DIR}/edges.c"
  output.cpp
  misc.cpp
  third_party/nlopt_slsqp/slsqp.c
  third_party/nlopt_slsqp/stop.c
  third_party/nlopt_slsqp/timer.c)

option(MERGEHDR_BUILD_LOCAL_VISIBILITY_COMPARISON
  "Build the machine-specific visibility comparison utility" OFF)
if(MERGEHDR_BUILD_LOCAL_VISIBILITY_COMPARISON)
  add_executable(compare_visibility_result
    build-mergehdr/compare_visibility_result.cpp
    camera_detect.cpp
    colorcalibrate.cpp
    chartcells.cpp
    glareeval.cpp
    hdrimage.cpp
    "${MERGEHDR_MEX_DIR}/convolve.c"
    "${MERGEHDR_MEX_DIR}/edges.c"
    output.cpp
    misc.cpp
    third_party/nlopt_slsqp/slsqp.c
    third_party/nlopt_slsqp/stop.c
    third_party/nlopt_slsqp/timer.c)
endif()

target_link_libraries(mergehdrcore ${CORESERVICES_LIBRARY}
  JPEG::JPEG Exiv2::exiv2lib
  Boost::headers Boost::filesystem Boost::program_options
  Eigen3::Eigen ${LIBRAW_LINK_LIBRARIES})

target_link_libraries(mergehdr
  ${CORESERVICES_LIBRARY}
  ${ACCELERATE_FRAMEWORK}
  ${FFTW3_LIBRARY}
  JPEG::JPEG
  Exiv2::exiv2lib
  Boost::headers Boost::filesystem Boost::program_options
  Eigen3::Eigen ${LIBRAW_LINK_LIBRARIES})
target_include_directories(mergehdr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlopt_slsqp)

if(TARGET compare_visibility_result)
  target_link_libraries(compare_visibility_result
    ${CORESERVICES_LIBRARY}
    ${ACCELERATE_FRAMEWORK}
    ${FFTW3_LIBRARY}
    JPEG::JPEG
    Exiv2::exiv2lib
    Boost::headers Boost::filesystem Boost::program_options
    Eigen3::Eigen ${LIBRAW_LINK_LIBRARIES})
  target_include_directories(compare_visibility_result PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlopt_slsqp)
endif()

if(MERGEHDR_ENABLE_OPENEXR)
  target_link_libraries(mergehdrcore Imath::Imath OpenEXR::OpenEXR)
  target_link_libraries(mergehdr Imath::Imath OpenEXR::OpenEXR)
  if(TARGET compare_visibility_result)
    target_link_libraries(compare_visibility_result Imath::Imath OpenEXR::OpenEXR)
  endif()
endif()

if(MERGEHDR_ENABLE_OPENEXR)
  target_compile_definitions(mergehdrcore PRIVATE MERGEHDR_ENABLE_OPENEXR=1)
  target_compile_definitions(mergehdr PRIVATE MERGEHDR_ENABLE_OPENEXR=1)
  if(TARGET compare_visibility_result)
    target_compile_definitions(compare_visibility_result PRIVATE MERGEHDR_ENABLE_OPENEXR=1)
  endif()
else()
  target_compile_definitions(mergehdrcore PRIVATE MERGEHDR_ENABLE_OPENEXR=0)
  target_compile_definitions(mergehdr PRIVATE MERGEHDR_ENABLE_OPENEXR=0)
  if(TARGET compare_visibility_result)
    target_compile_definitions(compare_visibility_result PRIVATE MERGEHDR_ENABLE_OPENEXR=0)
  endif()
endif()

if(MERGEHDR_ENABLE_OPENMP)
  target_compile_definitions(mergehdrcore PRIVATE MERGEHDR_ENABLE_OPENMP=1)
  target_compile_definitions(mergehdr PRIVATE MERGEHDR_ENABLE_OPENMP=1)
  if(TARGET compare_visibility_result)
    target_compile_definitions(compare_visibility_result PRIVATE MERGEHDR_ENABLE_OPENMP=1)
  endif()
else()
  target_compile_definitions(mergehdrcore PRIVATE MERGEHDR_ENABLE_OPENMP=0)
  target_compile_definitions(mergehdr PRIVATE MERGEHDR_ENABLE_OPENMP=0)
  if(TARGET compare_visibility_result)
    target_compile_definitions(compare_visibility_result PRIVATE MERGEHDR_ENABLE_OPENMP=0)
  endif()
endif()

if(MERGEHDR_ENABLE_OPENMP AND FFTW3_OMP_LIBRARY)
	target_link_libraries(mergehdr ${FFTW3_OMP_LIBRARY})
	if(TARGET compare_visibility_result)
		target_link_libraries(compare_visibility_result ${FFTW3_OMP_LIBRARY})
	endif()
endif()

if (MERGEHDR_ENABLE_OPENMP AND OpenMP_CXX_FOUND)
	target_link_libraries(mergehdrcore OpenMP::OpenMP_CXX)
	target_link_libraries(mergehdr OpenMP::OpenMP_CXX)
	if(TARGET compare_visibility_result)
		target_link_libraries(compare_visibility_result OpenMP::OpenMP_CXX)
	endif()
elseif(MERGEHDR_ENABLE_OPENMP AND APPLE AND LIBOMP_LIBRARY AND LIBOMP_INCLUDE_DIR)
	target_compile_options(mergehdrcore PRIVATE -Xpreprocessor -fopenmp)
	target_link_libraries(mergehdrcore ${LIBOMP_LIBRARY})
	target_compile_options(mergehdr PRIVATE -Xpreprocessor -fopenmp)
	target_link_libraries(mergehdr ${LIBOMP_LIBRARY})
	if(TARGET compare_visibility_result)
		target_compile_options(compare_visibility_result PRIVATE -Xpreprocessor -fopenmp)
		target_link_libraries(compare_visibility_result ${LIBOMP_LIBRARY})
	endif()
endif()

if((MERGEHDR_ENABLE_OPENEXR OR MERGEHDR_ENABLE_OPENMP) AND APPLE AND MERGEHDR_HOMEBREW_PREFIX AND EXISTS "${MERGEHDR_HOMEBREW_PREFIX}/lib")
	set_target_properties(mergehdrcore PROPERTIES
		BUILD_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib"
		INSTALL_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib")
	set_target_properties(mergehdr PROPERTIES
		BUILD_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib"
		INSTALL_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib")
	if(TARGET compare_visibility_result)
		set_target_properties(compare_visibility_result PROPERTIES
			BUILD_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib"
			INSTALL_RPATH "${MERGEHDR_HOMEBREW_PREFIX}/lib")
	endif()
endif()
