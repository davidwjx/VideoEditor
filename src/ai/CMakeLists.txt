find_package(FFmpeg REQUIRED)

add_library(videoeditor_ai STATIC
  AiProtocol.h AiProtocol.cpp
  AiTransport.h
  LocalAiTransport.h LocalAiTransport.cpp
  AiJobManager.h AiJobManager.cpp
  ModelRegistry.h ModelRegistry.cpp
  AiFeatureGate.h AiFeatureGate.cpp
  engine/Confidence.h engine/Confidence.cpp
  engine/FrameReader.h engine/FrameReader.cpp
)
target_include_directories(videoeditor_ai PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_link_libraries(videoeditor_ai PUBLIC
  videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Network
  FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale)
target_include_directories(videoeditor_ai PRIVATE ${FFMPEG_INCLUDE_DIR})

option(VIDEOEDITOR_ENABLE_AI "Build local AI removal engine" ON)

# Flow propagation only needs OpenCV; it is compiled separately so the
# VIDEOEDITOR_HAVE_ORT=0 configuration can still run the DIS unit tests.
set(VIDEOEDITOR_AI_HAVE_OPENCV OFF)
set(VIDEOEDITOR_AI_HAVE_ORT OFF)
if(VIDEOEDITOR_OPENCV_ROOT
   AND EXISTS "${VIDEOEDITOR_OPENCV_ROOT}/include/opencv2/optflow.hpp"
   AND EXISTS "${VIDEOEDITOR_OPENCV_ROOT}/lib/opencv_core320.lib")
  set(VIDEOEDITOR_AI_HAVE_OPENCV ON)
  target_sources(videoeditor_ai PRIVATE
    engine/FlowPropagator.h engine/FlowPropagator.cpp)
  target_include_directories(videoeditor_ai PRIVATE
    "${VIDEOEDITOR_OPENCV_ROOT}/include")
  target_link_libraries(videoeditor_ai PRIVATE
    "${VIDEOEDITOR_OPENCV_ROOT}/lib/opencv_core320.lib"
    "${VIDEOEDITOR_OPENCV_ROOT}/lib/opencv_imgproc320.lib"
    "${VIDEOEDITOR_OPENCV_ROOT}/lib/opencv_video320.lib"
    "${VIDEOEDITOR_OPENCV_ROOT}/lib/opencv_optflow320.lib")
endif()

if(VIDEOEDITOR_ENABLE_AI)
  add_executable(VisionAiEngine AiEngineMain.cpp)
  target_link_libraries(VisionAiEngine PRIVATE videoeditor_ai videoeditor_media Qt6::Gui)
  videoeditor_copy_ai_runtime(VisionAiEngine)

  if(VIDEOEDITOR_AI_HAVE_OPENCV
     AND VIDEOEDITOR_ONNXRUNTIME_ROOT
     AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h"
     AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
     AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/bin/onnxruntime.dll")
    set(VIDEOEDITOR_AI_HAVE_ORT ON)
    # ORT-only engine sources: tracking, inpainting and temporal stabilization.
    target_sources(videoeditor_ai PRIVATE
      engine/OnnxExecutor.h engine/OnnxExecutor.cpp
      engine/Sam2Tracker.h engine/Sam2Tracker.cpp
      engine/TrackingPipeline.h engine/TrackingPipeline.cpp
      engine/InpaintPipeline.h engine/InpaintPipeline.cpp
      engine/MiGanInpainter.h engine/MiGanInpainter.cpp
      engine/LamaInpainter.h engine/LamaInpainter.cpp
      engine/TemporalStabilizer.h engine/TemporalStabilizer.cpp)
    target_include_directories(videoeditor_ai PRIVATE
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include")
    target_link_libraries(videoeditor_ai PRIVATE
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
  endif()
endif()

if(VIDEOEDITOR_AI_HAVE_OPENCV)
  set(VIDEOEDITOR_AI_HAVE_OPENCV_VALUE 1)
else()
  set(VIDEOEDITOR_AI_HAVE_OPENCV_VALUE 0)
endif()
if(VIDEOEDITOR_AI_HAVE_ORT)
  set(VIDEOEDITOR_AI_HAVE_ORT_VALUE 1)
else()
  set(VIDEOEDITOR_AI_HAVE_ORT_VALUE 0)
endif()
target_compile_definitions(videoeditor_ai PUBLIC
  VIDEOEDITOR_HAVE_OPENCV=${VIDEOEDITOR_AI_HAVE_OPENCV_VALUE}
  VIDEOEDITOR_HAVE_ORT=${VIDEOEDITOR_AI_HAVE_ORT_VALUE})
