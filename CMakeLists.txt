cmake_minimum_required(VERSION 3.16)
project(sonshell LANGUAGES CXX)

# Path to the extracted Sony SDK (folder that contains "app" and "external")
set(SONY_SDK_DIR "" CACHE PATH "Path to the root of Sony Camera Remote SDK package")
if(NOT SONY_SDK_DIR)
  message(FATAL_ERROR "Please set SONY_SDK_DIR to your extracted SDK path")
endif()

# Shortcuts to SDK dirs
set(CRSDK_ROOT      "${SONY_SDK_DIR}/external/crsdk")
set(CRSDK_ADAPTER   "${CRSDK_ROOT}/CrAdapter")

# Headers live in the SDK’s "app" folder for this package layout
include_directories(
  "${SONY_SDK_DIR}/app"
  "${SONY_SDK_DIR}/external/opencv/include"
  "${CMAKE_BINARY_DIR}/gen"
)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GENERATED_DIR "${CMAKE_BINARY_DIR}/gen")
set(PROP_HEADER   "${SONY_SDK_DIR}/app/CRSDK/CrDeviceProperty.h")
set(ERROR_HEADER  "${SONY_SDK_DIR}/app/CRSDK/CrError.h")

if(NOT EXISTS "${PROP_HEADER}")
  message(FATAL_ERROR "Missing required SDK header: ${PROP_HEADER}")
endif()
if(NOT EXISTS "${ERROR_HEADER}")
  message(FATAL_ERROR "Missing required SDK header: ${ERROR_HEADER}")
endif()

set(PROP_GENERATED  "${GENERATED_DIR}/prop_names_generated.h")
set(ERROR_GENERATED "${GENERATED_DIR}/error_names_generated.h")

add_custom_command(
  OUTPUT "${PROP_GENERATED}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_prop_names.py"
          --header "${PROP_HEADER}" -o "${PROP_GENERATED}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_prop_names.py" "${PROP_HEADER}"
  COMMENT "Generating prop_names_generated.h from CrDeviceProperty.h"
  VERBATIM
)

add_custom_command(
  OUTPUT "${ERROR_GENERATED}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen_error_names.py"
          --header "${ERROR_HEADER}" -o "${ERROR_GENERATED}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_error_names.py" "${ERROR_HEADER}"
  COMMENT "Generating error_names_generated.h from CrError.h"
  VERBATIM
)

add_custom_target(sonshell_generate_sdk_maps
  DEPENDS "${PROP_GENERATED}" "${ERROR_GENERATED}"
)

add_executable(sonshell src/main.cpp)
add_dependencies(sonshell sonshell_generate_sdk_maps)

# --- RPATH / RUNPATH configuration ------------------------------------------
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

# Keep runtime search paths relative to the executable
set(_rpaths
  "$ORIGIN"
  "$ORIGIN/CrAdapter"
)
set_target_properties(sonshell PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
  BUILD_RPATH   "${_rpaths}"
  INSTALL_RPATH "${_rpaths}"
)

# Extra safety: also pass rpath flags directly to the linker
target_link_options(sonshell PRIVATE
  "-Wl,-rpath,$ORIGIN"
  "-Wl,-rpath,$ORIGIN/CrAdapter"
)

# --- Linking ----------------------------------------------------------------

find_library(EDIT_LIB edit REQUIRED)
find_library(NCURSES_LIB ncurses REQUIRED)  # libedit depends on curses

# Turn off --as-needed around OpenCV libs so they remain DT_NEEDED even if only transitively required
target_link_libraries(sonshell
  PRIVATE
    "${CRSDK_ROOT}/libCr_Core.so"
    "${CRSDK_ADAPTER}/libCr_PTP_IP.so"

    -Wl,--no-as-needed
    ${SONY_SDK_DIR}/external/opencv/Linux/libopencv_core.so.408
    ${SONY_SDK_DIR}/external/opencv/Linux/libopencv_highgui.so.408
    ${SONY_SDK_DIR}/external/opencv/Linux/libopencv_imgcodecs.so.408
    ${SONY_SDK_DIR}/external/opencv/Linux/libopencv_imgproc.so.408
    -Wl,--as-needed

    ${EDIT_LIB}
    ${NCURSES_LIB}

    dl pthread
)

# Sony's headers alias CrInt8 to plain `char`, which is unsigned on ARM.
# Force signed chars so enums with negative values (e.g. CrZoomOperation_Wide)
# compile consistently across x86_64 and aarch64 toolchains.
target_compile_options(sonshell PRIVATE -fsigned-char)

# --- Post-build copy helpers -------------------------------------------------
function(copy_dir_if_exists SRC DST)
  if(EXISTS "${SRC}")
    add_custom_command(TARGET sonshell POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${DST}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SRC}" "${DST}"
    )
  else()
    message(STATUS "Skip copy (not found): ${SRC}")
  endif()
endfunction()

function(copy_file_if_exists SRC DST_DIR)
  if(EXISTS "${SRC}")
    add_custom_command(TARGET sonshell POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC}" "${DST_DIR}"
    )
  else()
    message(STATUS "Skip copy (not found): ${SRC}")
  endif()
endfunction()

# Core + monitor protocol libs
copy_file_if_exists("${CRSDK_ROOT}/libCr_Core.so"              "$<TARGET_FILE_DIR:sonshell>")
copy_file_if_exists("${CRSDK_ROOT}/libmonitor_protocol.so"     "$<TARGET_FILE_DIR:sonshell>")
copy_file_if_exists("${CRSDK_ROOT}/libmonitor_protocol_pf.so"  "$<TARGET_FILE_DIR:sonshell>")

# Entire CrAdapter directory (brings libCr_PTP_IP.so, libssh2.so, libusb-1.0.so, etc.)
copy_dir_if_exists("${CRSDK_ADAPTER}" "$<TARGET_FILE_DIR:sonshell>/CrAdapter")

# Copy all OpenCV libs (and potential 3rd-party deps) next to the binary
file(GLOB OPENCV_ALL_SO "${SONY_SDK_DIR}/external/opencv/Linux/*.so*")
foreach(OPCV_SO ${OPENCV_ALL_SO})
  copy_file_if_exists("${OPCV_SO}" "$<TARGET_FILE_DIR:sonshell>")
endforeach()

message(STATUS "Configured with SONY_SDK_DIR=${SONY_SDK_DIR}")
