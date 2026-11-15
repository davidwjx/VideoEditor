cmake_minimum_required(VERSION 3.27)
project(VideoEditor VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
  add_compile_options(/utf-8)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Qml Network Test)

find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)
  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest ALIAS gtest)
  endif()
  if(NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
  endif()
endif()

qt_standard_project_setup(REQUIRES 6.8)

set(VIDEOEDITOR_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime root")
set(VIDEOEDITOR_OPENCV_ROOT "" CACHE PATH "OpenCV root")

# ONNX Runtime 1.20 and the OpenCV DLLs must sit next to every executable that
# loads them: the Windows System32 onnxruntime.dll (1.17.1) otherwise wins.
function(videoeditor_copy_ai_runtime target)
  if(NOT TARGET ${target})
    return()
  endif()
  if(VIDEOEDITOR_ONNXRUNTIME_ROOT
     AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/bin/onnxruntime.dll")
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/bin/onnxruntime.dll"
              "$<TARGET_FILE_DIR:${target}>")
  endif()
  if(VIDEOEDITOR_OPENCV_ROOT
     AND EXISTS "${VIDEOEDITOR_OPENCV_ROOT}/bin")
    # optflow pulls in ximgproc/flann/videoio/imgcodecs/calib3d/features2d;
    # copy the whole closure next to each executable.
    foreach(_ai_dll opencv_core320 opencv_imgproc320 opencv_video320
                     opencv_optflow320 opencv_ximgproc320 opencv_flann320
                     opencv_videoio320 opencv_imgcodecs320 opencv_calib3d320
                     opencv_features2d320)
      if(EXISTS "${VIDEOEDITOR_OPENCV_ROOT}/bin/${_ai_dll}.dll")
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${VIDEOEDITOR_OPENCV_ROOT}/bin/${_ai_dll}.dll"
                  "$<TARGET_FILE_DIR:${target}>")
      endif()
    endforeach()
  endif()
endfunction()

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)
