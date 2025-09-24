cmake_minimum_required(VERSION 3.16)
project(sony_connect_only LANGUAGES CXX)

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
  "${CMAKE_BINARY_DIR}/gen"
)

add_executable(sony_connect_only
  src/main.cpp
)

# Ensure runtime loader finds libs next to the binary + the subfolders the SDK probes
# ($ORIGIN is the dir of the built binary)
set(_rpaths
  "$ORIGIN"
  "$ORIGIN/CrAdapter"
  "$ORIGIN/glibc-hwcaps/x86-64-v3"
  "$ORIGIN/glibc-hwcaps/x86-64-v2"
  "$ORIGIN/CrAdapter/glibc-hwcaps/x86-64-v3"
  "$ORIGIN/CrAdapter/glibc-hwcaps/x86-64-v2"
)
string (REPLACE ";" ":" _rpath_str "${_rpaths}")
set_target_properties(sony_connect_only PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
  CXX_EXTENSIONS NO
  BUILD_RPATH   "${_rpath_str}"
  INSTALL_RPATH "${_rpath_str}"
)

# Link against SDK core + IP adaptor
target_link_libraries(sony_connect_only
  PRIVATE
    "${CRSDK_ROOT}/libCr_Core.so"
    "${CRSDK_ADAPTER}/libCr_PTP_IP.so"
    dl pthread
)

# Helper macro: copy dir if it exists
function(copy_dir_if_exists SRC DST)
  if(EXISTS "${SRC}")
    add_custom_command(TARGET sony_connect_only POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "${DST}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${SRC}" "${DST}"
    )
  else()
    message(STATUS "Skip copy (not found): ${SRC}")
  endif()
endfunction()

# Helper macro: copy file if it exists
function(copy_file_if_exists SRC DST_DIR)
  if(EXISTS "${SRC}")
    add_custom_command(TARGET sony_connect_only POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC}" "${DST_DIR}"
    )
  else()
    message(STATUS "Skip copy (not found): ${SRC}")
  endif()
endfunction()

# Core + monitor protocol libs
copy_file_if_exists("${CRSDK_ROOT}/libCr_Core.so"              "$<TARGET_FILE_DIR:sony_connect_only>")
copy_file_if_exists("${CRSDK_ROOT}/libmonitor_protocol.so"     "$<TARGET_FILE_DIR:sony_connect_only>")
copy_file_if_exists("${CRSDK_ROOT}/libmonitor_protocol_pf.so"  "$<TARGET_FILE_DIR:sony_connect_only>")

# Entire CrAdapter directory (brings libCr_PTP_IP.so, libssh2.so, libusb-1.0.so, etc.)
copy_dir_if_exists("${CRSDK_ADAPTER}" "$<TARGET_FILE_DIR:sony_connect_only>/CrAdapter")

# glibc-hwcaps trees the loader may probe (top-level and under CrAdapter)
copy_dir_if_exists("${CRSDK_ROOT}/glibc-hwcaps/x86-64-v3" "$<TARGET_FILE_DIR:sony_connect_only>/glibc-hwcaps/x86-64-v3")
copy_dir_if_exists("${CRSDK_ROOT}/glibc-hwcaps/x86-64-v2" "$<TARGET_FILE_DIR:sony_connect_only>/glibc-hwcaps/x86-64-v2")
copy_dir_if_exists("${CRSDK_ADAPTER}/glibc-hwcaps/x86-64-v3" "$<TARGET_FILE_DIR:sony_connect_only>/CrAdapter/glibc-hwcaps/x86-64-v3")
copy_dir_if_exists("${CRSDK_ADAPTER}/glibc-hwcaps/x86-64-v2" "$<TARGET_FILE_DIR:sony_connect_only>/CrAdapter/glibc-hwcaps/x86-64-v2")

# (Optional) Copy OpenCV libs bundled with SDK (not strictly needed)
copy_file_if_exists("${SONY_SDK_DIR}/external/opencv/Linux/libopencv_core.so.408"      "$<TARGET_FILE_DIR:sony_connect_only>")
copy_file_if_exists("${SONY_SDK_DIR}/external/opencv/Linux/libopencv_highgui.so.408"   "$<TARGET_FILE_DIR:sony_connect_only>")
copy_file_if_exists("${SONY_SDK_DIR}/external/opencv/Linux/libopencv_imgcodecs.so.408" "$<TARGET_FILE_DIR:sony_connect_only>")
copy_file_if_exists("${SONY_SDK_DIR}/external/opencv/Linux/libopencv_imgproc.so.408"   "$<TARGET_FILE_DIR:sony_connect_only>")

message(STATUS "Configured with SONY_SDK_DIR=${SONY_SDK_DIR}")
