cmake_minimum_required(VERSION 3.20)
project(kinect_viewer_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Where libfreenect2 is installed (include/ and lib/ under it). Pass -DFREENECT2_PREFIX=<that folder> to use
# another. The defaults: on Windows, libfreenect2\install in the folder that holds this checkout, as
# windows/scripts/build.cmd and docs/windows-setup.md lay it out; elsewhere ~/libfreenect2, as Config/Base.xcconfig.
if(WIN32)
    get_filename_component(_kv_default_freenect2_prefix "${CMAKE_CURRENT_SOURCE_DIR}/../libfreenect2/install" ABSOLUTE)
else()
    set(_kv_default_freenect2_prefix "$ENV{HOME}/libfreenect2")
endif()
set(FREENECT2_PREFIX "${_kv_default_freenect2_prefix}" CACHE PATH "Prefix where libfreenect2 is installed")

find_path(FREENECT2_INCLUDE_DIR
    NAMES libfreenect2/libfreenect2.hpp
    HINTS "${FREENECT2_PREFIX}/include"
)

find_library(FREENECT2_LIBRARY
    NAMES freenect2
    HINTS "${FREENECT2_PREFIX}/lib"
)

if(NOT FREENECT2_INCLUDE_DIR OR NOT FREENECT2_LIBRARY)
    message(FATAL_ERROR "Could not locate libfreenect2 under ${FREENECT2_PREFIX}: pass -DFREENECT2_PREFIX=<the folder "
        "it is installed in> (README.md, docs/windows-setup.md)")
endif()

add_library(kinect_core STATIC
    core/src/colormaps.cpp
    core/src/frame_mapper.cpp
    core/src/synthetic_source.cpp
    core/src/freenect2_source.cpp
    core/src/lock_guard_file.cpp
    core/src/kinect_core_c.cpp
)

target_include_directories(kinect_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/core/include"
    PRIVATE
        "${FREENECT2_INCLUDE_DIR}"
)

target_link_libraries(kinect_core
    PUBLIC
        "${FREENECT2_LIBRARY}"
)

if(APPLE)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    target_link_libraries(kinect_core PUBLIC
        "${COREFOUNDATION_FRAMEWORK}"
        "${IOKIT_FRAMEWORK}"
    )
endif()

if(WIN32)
    # Windows host (CLI, viewer, iPhone streaming). See docs/windows-setup.md.
    target_compile_definitions(kinect_core PRIVATE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(kinect_core PRIVATE /utf-8 /W3)
    endif()
    target_link_libraries(kinect_core PUBLIC setupapi)
    enable_testing()
    add_subdirectory(windows)
endif()
