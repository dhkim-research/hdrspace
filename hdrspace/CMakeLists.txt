cmake_minimum_required(VERSION 3.16)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS deployment target" FORCE)
endif()

project(hdrspace VERSION 1.0.0 LANGUAGES C CXX)

set(HDRSPACE_APP_NAME "hdrspace")
set(HDRSPACE_GUI_IDENTIFIER "com.hdrspace.app")
set(HDRSPACE_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/hdrspace.icns")
set(HDRSPACE_BRAND_PNGS
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/hdrspace_icon_1024.png"
)
set(HDRSPACE_BRAND_MISC
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/hdrspace.svg"
)
set(HDRSPACE_BASELINE_APP "${CMAKE_CURRENT_SOURCE_DIR}/../hdrspace-baseline/payload" CACHE PATH
    "Read-only baseline app bundle supplying the validated helper/runtime payload")
set(HDRSPACE_STAGE_DIR "${CMAKE_BINARY_DIR}/../app" CACHE PATH
    "Staging directory for hdrspace.app; never an Applications directory")
set(HDRSPACE_LICENSES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/licenses" CACHE PATH
    "License texts copied to Contents/Resources/licenses (tools/collect_hdrspace_licenses.py)")
set(HDRSPACE_MERGEHDR_BUILD_DIR "" CACHE PATH
    "Optional folder with rebuilt mergehdr and mergehdrcore that replace the baseline pair")

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B /temporary/workspace/build")
endif()
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Production macOS architecture" FORCE)
endif()
option(HDRSPACE_ENABLE_OPENMP "Enable OpenMP acceleration in the app build" ON)
set(HDRSPACE_HDRVDP3_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/hdrvdp3-data" CACHE PATH
    "Local HDR-VDP3 data snapshot; packaged payload must match the baseline")
set(HDRSPACE_MERGEHDR_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../mergehdr" CACHE PATH
    "Canonical mergehdr source directory (read-only)")
if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
  find_library(ACCELERATE_FRAMEWORK Accelerate)
endif()
# Link against the validated app's dylibs, not whichever versions a package
# manager happens to provide today. The packager copies these files unchanged.
find_path(LIBOMP_INCLUDE_DIR omp.h HINTS /opt/homebrew/opt/libomp/include)
find_path(FFTW3_INCLUDE_DIR fftw3.h HINTS /opt/homebrew/opt/fftw/include /opt/homebrew/include /usr/local/include)
if(APPLE)
  foreach(HDRSPACE_LIBRARY IN ITEMS libfftw3.3.dylib libfftw3_omp.3.dylib libomp.dylib)
    if(NOT EXISTS "${HDRSPACE_BASELINE_APP}/Contents/Frameworks/${HDRSPACE_LIBRARY}")
      message(FATAL_ERROR "Baseline runtime library is missing: ${HDRSPACE_LIBRARY}")
    endif()
  endforeach()
  set(FFTW3_LIBRARY "${HDRSPACE_BASELINE_APP}/Contents/Frameworks/libfftw3.3.dylib")
  set(FFTW3_OMP_LIBRARY "${HDRSPACE_BASELINE_APP}/Contents/Frameworks/libfftw3_omp.3.dylib")
  set(LIBOMP_LIBRARY "${HDRSPACE_BASELINE_APP}/Contents/Frameworks/libomp.dylib")
else()
  find_library(FFTW3_LIBRARY NAMES fftw3.3 fftw3 REQUIRED)
  find_library(FFTW3_THREADS_LIBRARY NAMES fftw3_threads.3 fftw3_threads)
  if(HDRSPACE_ENABLE_OPENMP)
    find_package(OpenMP)
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HDRSPACE_SOURCE_DIR_RAW "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(HDRSPACE_SOURCE_DIR_REAL "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
set(TEV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tev")
set(MERGEHDR_SOURCE_DIR "${HDRSPACE_MERGEHDR_SOURCE_DIR}")
get_filename_component(TEV_SOURCE_DIR_REAL "${TEV_SOURCE_DIR}" REALPATH)
get_filename_component(MERGEHDR_SOURCE_DIR_REAL "${MERGEHDR_SOURCE_DIR}" REALPATH)
set(TEV_DEPENDENCIES_DIR "${TEV_SOURCE_DIR}/dependencies")
set(HDRVDP_MEX_DIR "" CACHE PATH "Path to the HDR-VDP matlabPyrTools MEX sources")
if(NOT HDRVDP_MEX_DIR)
  set(HDRVDP_MEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/hdrvdp-mex")
endif()
get_filename_component(HDRVDP_MEX_DIR_REAL "${HDRVDP_MEX_DIR}" REALPATH)
foreach(HDRSPACE_REQUIRED_FILE IN ITEMS
    "${MERGEHDR_SOURCE_DIR}/glareeval.cpp"
    "${HDRVDP_MEX_DIR}/convolve.c"
    "${HDRSPACE_HDRVDP3_DATA_DIR}/emission_spectra_oled.csv")
  if(NOT EXISTS "${HDRSPACE_REQUIRED_FILE}")
    message(FATAL_ERROR "Required local source/data file is missing: ${HDRSPACE_REQUIRED_FILE}")
  endif()
endforeach()

if(DEFINED ENV{HOME} AND NOT MSVC)
  add_compile_options(
    "-ffile-prefix-map=${HDRSPACE_SOURCE_DIR_RAW}=/src/hdrspace"
    "-fmacro-prefix-map=${HDRSPACE_SOURCE_DIR_RAW}=/src/hdrspace"
    "-ffile-prefix-map=${HDRSPACE_SOURCE_DIR_REAL}=/src/hdrspace"
    "-fmacro-prefix-map=${HDRSPACE_SOURCE_DIR_REAL}=/src/hdrspace"
    "-ffile-prefix-map=${TEV_SOURCE_DIR}=/src/tev"
    "-fmacro-prefix-map=${TEV_SOURCE_DIR}=/src/tev"
    "-ffile-prefix-map=${TEV_SOURCE_DIR_REAL}=/src/tev"
    "-fmacro-prefix-map=${TEV_SOURCE_DIR_REAL}=/src/tev"
    "-ffile-prefix-map=${MERGEHDR_SOURCE_DIR}=/src/mergehdr"
    "-fmacro-prefix-map=${MERGEHDR_SOURCE_DIR}=/src/mergehdr"
    "-ffile-prefix-map=${MERGEHDR_SOURCE_DIR_REAL}=/src/mergehdr"
    "-fmacro-prefix-map=${MERGEHDR_SOURCE_DIR_REAL}=/src/mergehdr"
    "-ffile-prefix-map=${HDRVDP_MEX_DIR}=/src/hdrvdp-mex"
    "-fmacro-prefix-map=${HDRVDP_MEX_DIR}=/src/hdrvdp-mex"
    "-ffile-prefix-map=${HDRVDP_MEX_DIR_REAL}=/src/hdrvdp-mex"
    "-fmacro-prefix-map=${HDRVDP_MEX_DIR_REAL}=/src/hdrvdp-mex"
    "-ffile-prefix-map=$ENV{HOME}=/home"
    "-fmacro-prefix-map=$ENV{HOME}=/home"
  )
endif()

find_package(Eigen3 CONFIG REQUIRED)

set(TEV_SUPPORT_AVIF OFF CACHE BOOL "" FORCE)
set(TEV_SUPPORT_HEIC OFF CACHE BOOL "" FORCE)
set(TEV_SUPPORT_JXL ON CACHE BOOL "" FORCE)
set(TEV_USE_LIBHEIF OFF CACHE BOOL "" FORCE)
add_subdirectory("${TEV_DEPENDENCIES_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tev-dependencies" EXCLUDE_FROM_ALL)

file(GLOB TEV_IMAGEIO_SOURCES
  "${TEV_SOURCE_DIR}/src/imageio/*.cpp"
)

set(TEV_CORE_SOURCES
  "${TEV_SOURCE_DIR}/src/BackgroundImagesLoader.cpp"
  "${TEV_SOURCE_DIR}/src/Box.cpp"
  "${TEV_SOURCE_DIR}/src/Channel.cpp"
  "${TEV_SOURCE_DIR}/src/Common.cpp"
  "${TEV_SOURCE_DIR}/src/FalseColor.cpp"
  "${TEV_SOURCE_DIR}/src/HelpWindow.cpp"
  "${TEV_SOURCE_DIR}/src/Image.cpp"
  "${TEV_SOURCE_DIR}/src/ImageButton.cpp"
  "${TEV_SOURCE_DIR}/src/Lazy.cpp"
  "${TEV_SOURCE_DIR}/src/ImageInfoWindow.cpp"
  "${TEV_SOURCE_DIR}/src/ImageViewer.cpp"
  "${TEV_SOURCE_DIR}/src/Ipc.cpp"
  "${TEV_SOURCE_DIR}/src/MultiGraph.cpp"
  "${TEV_SOURCE_DIR}/src/ProfileGraph.cpp"
  "${TEV_SOURCE_DIR}/src/SharedQueue.cpp"
  "${TEV_SOURCE_DIR}/src/Task.cpp"
  "${TEV_SOURCE_DIR}/src/ThreadPool.cpp"
  "${TEV_SOURCE_DIR}/src/UberShader.cpp"
  "${TEV_SOURCE_DIR}/src/VectorGraphics.cpp"
  "${TEV_SOURCE_DIR}/src/WaylandClipboard.cpp"
  "${TEV_SOURCE_DIR}/src/ImageCanvas.cpp"
  ${TEV_IMAGEIO_SOURCES}
)

list(REMOVE_ITEM TEV_CORE_SOURCES
  "${TEV_SOURCE_DIR}/src/imageio/DdsImageLoader.cpp"
  "${TEV_SOURCE_DIR}/src/imageio/HeifImageLoader.cpp"
)

add_executable(hdrspace
  MACOSX_BUNDLE
  src/main.cpp
  src/native/Hdrspace.cpp
  src/native/GazeToneMapping.cpp
  src/native/ImageStatistics.cpp
  src/native/PfsToneMapping.cpp
  src/platform/ProcessRunner.cpp
  "${MERGEHDR_SOURCE_DIR}/glareeval.cpp"
  "${MERGEHDR_SOURCE_DIR}/hdrimage.cpp"
  "${HDRVDP_MEX_DIR}/convolve.c"
  "${HDRVDP_MEX_DIR}/edges.c"
  "${HDRSPACE_ICON}"
  ${HDRSPACE_BRAND_PNGS}
  ${HDRSPACE_BRAND_MISC}
  ${TEV_CORE_SOURCES}
)

if(WIN32)
  # Process and path helpers, the UTF-8 manifest and the window/taskbar icon.
  target_sources(hdrspace PRIVATE
    src/platform/WinPosix.cpp
    resources/windows/icon.rc
    resources/windows/hdrspace.manifest
    "${TEV_SOURCE_DIR}/src/imageio/DdsImageLoader.cpp")
  # tev reads DDS through DirectXTex on Windows (built with the tev dependencies).
  target_link_libraries(hdrspace PRIVATE DirectXTex)
endif()

set_source_files_properties("${HDRSPACE_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
set_source_files_properties(${HDRSPACE_BRAND_PNGS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/branding")
set_source_files_properties(${HDRSPACE_BRAND_MISC} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/branding")

if(MSVC)
  target_compile_options(hdrspace PRIVATE /W1 /utf-8 /bigobj /permissive- /Zc:__cplusplus)
  target_compile_definitions(hdrspace PRIVATE
    _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  target_link_libraries(hdrspace PRIVATE wsock32 ws2_32)
else()
  target_compile_options(hdrspace PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-shadow
  )
endif()

target_compile_definitions(hdrspace PRIVATE
  TEV_VERSION="hdrspace"
  HDRSPACE_VERSION="${PROJECT_VERSION}"
  GL_SILENCE_DEPRECATION
  TEV_SUPPORT_JXL
  MERGEHDR_ENABLE_EXPERIMENTAL_PERCEPTUAL_MAPS
)

set_target_properties(hdrspace PROPERTIES
  OUTPUT_NAME "${HDRSPACE_APP_NAME}"
  MACOSX_BUNDLE_BUNDLE_NAME "${HDRSPACE_APP_NAME}"
  MACOSX_BUNDLE_BUNDLE_EXECUTABLE_NAME "${HDRSPACE_APP_NAME}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${HDRSPACE_GUI_IDENTIFIER}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_ICON_FILE "hdrspace.icns"
)

target_include_directories(hdrspace PRIVATE
  src
  "${MERGEHDR_SOURCE_DIR}"
  "${HDRVDP_MEX_DIR}"
  "${FFTW3_INCLUDE_DIR}"
  "${TEV_SOURCE_DIR}/include"
  "${TEV_SOURCE_DIR}/dependencies"
  "${TEV_SOURCE_DIR}/dependencies/args"
  "${TEV_SOURCE_DIR}/dependencies/qoi"
  "${TEV_SOURCE_DIR}/dependencies/small_vector/source/include"
  "${TEV_SOURCE_DIR}/dependencies/stb"
  "${TEV_SOURCE_DIR}/dependencies/tinylogger"
  "${TEV_SOURCE_DIR}/dependencies/utfcpp/source"
)
if(LIBOMP_INCLUDE_DIR)
  target_include_directories(hdrspace PRIVATE ${LIBOMP_INCLUDE_DIR})
endif()

target_link_libraries(hdrspace PRIVATE
  ${ACCELERATE_FRAMEWORK}
  ${FFTW3_LIBRARY}
  clip
  Eigen3::Eigen
  exif_static
  jpeg-turbo
  libraw::libraw_r
  lcms2-tev
  nanogui
  OpenEXR::OpenEXR
  openjpeg-tev
  png_static
  tiff
  webpdemux
  XMPCore
  jxl
  jxl_dec
  jxl_extras_nocodec-internal
  jxl_threads
)
if(FFTW3_OMP_LIBRARY)
  target_link_libraries(hdrspace PRIVATE ${FFTW3_OMP_LIBRARY})
elseif(FFTW3_THREADS_LIBRARY)
  target_link_libraries(hdrspace PRIVATE ${FFTW3_THREADS_LIBRARY})
endif()
if(HDRSPACE_ENABLE_OPENMP AND APPLE)
  if(NOT LIBOMP_INCLUDE_DIR)
    message(FATAL_ERROR "OpenMP headers are required to preserve the accelerated native analysis path.")
  endif()
  target_compile_options(hdrspace PRIVATE -Xpreprocessor -fopenmp)
  target_link_libraries(hdrspace PRIVATE ${LIBOMP_LIBRARY})
elseif(HDRSPACE_ENABLE_OPENMP AND OpenMP_CXX_FOUND)
  target_link_libraries(hdrspace PRIVATE OpenMP::OpenMP_CXX)
endif()

if(APPLE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  # The linked GUI bundle is a build artifact. The final runnable app is staged
  # from the known-good baseline; helpers, dylibs, profiles and data are never
  # collected from package managers or rebuilt as a side effect of this target.
  set(HDRSPACE_PACKAGE_EXTRA_ARGS)
  if(HDRSPACE_MERGEHDR_BUILD_DIR)
    list(APPEND HDRSPACE_PACKAGE_EXTRA_ARGS --mergehdr-dir "${HDRSPACE_MERGEHDR_BUILD_DIR}")
    # Relink, and so re-stage, the app whenever the rebuilt helpers change.
    set_property(TARGET hdrspace APPEND PROPERTY LINK_DEPENDS
      "${HDRSPACE_MERGEHDR_BUILD_DIR}/mergehdr" "${HDRSPACE_MERGEHDR_BUILD_DIR}/mergehdrcore")
  endif()
  add_custom_command(TARGET hdrspace POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/package_hdrspace.py"
      --built-app "$<TARGET_BUNDLE_DIR:hdrspace>"
      --baseline-app "${HDRSPACE_BASELINE_APP}"
      --stage-dir "${HDRSPACE_STAGE_DIR}"
      --icon "${HDRSPACE_ICON}"
      --brand-png "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/hdrspace_icon_1024.png"
      --brand-svg "${CMAKE_CURRENT_SOURCE_DIR}/assets/branding/hdrspace.svg"
      --hdrvdp-data "${HDRSPACE_HDRVDP3_DATA_DIR}"
      --licenses "${HDRSPACE_LICENSES_DIR}"
      ${HDRSPACE_PACKAGE_EXTRA_ARGS}
    VERBATIM
  )
endif()
