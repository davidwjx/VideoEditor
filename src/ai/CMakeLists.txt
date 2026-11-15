add_library(videoeditor_ai STATIC
  AiProtocol.h AiProtocol.cpp
  AiTransport.h
  LocalAiTransport.h LocalAiTransport.cpp
  AiJobManager.h AiJobManager.cpp
  ModelRegistry.h ModelRegistry.cpp
  AiFeatureGate.h AiFeatureGate.cpp
)
target_include_directories(videoeditor_ai PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ai PUBLIC
  videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Network)

option(VIDEOEDITOR_ENABLE_AI "Build local AI removal engine" ON)
set(VIDEOEDITOR_ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime root")
if(VIDEOEDITOR_ENABLE_AI)
  add_executable(VisionAiEngine AiEngineMain.cpp)
  target_link_libraries(VisionAiEngine PRIVATE videoeditor_ai videoeditor_media Qt6::Gui)
  if(VIDEOEDITOR_ONNXRUNTIME_ROOT AND EXISTS "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    target_sources(videoeditor_ai PRIVATE
      engine/OnnxExecutor.h engine/OnnxExecutor.cpp
      engine/Confidence.h engine/Confidence.cpp
      engine/Sam2Tracker.h engine/Sam2Tracker.cpp
      engine/TrackingPipeline.h engine/TrackingPipeline.cpp
      engine/MiGanInpainter.h engine/MiGanInpainter.cpp
      engine/LamaInpainter.h engine/LamaInpainter.cpp
      engine/InpaintPipeline.h engine/InpaintPipeline.cpp
      engine/TemporalStabilizer.h engine/TemporalStabilizer.cpp
      engine/FrameReader.h engine/FrameReader.cpp)
    target_compile_definitions(videoeditor_ai PUBLIC VIDEOEDITOR_HAVE_ORT=1)
    target_include_directories(videoeditor_ai PUBLIC
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/include")
    target_link_libraries(videoeditor_ai PUBLIC
      "${VIDEOEDITOR_ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
  else()
    target_compile_definitions(videoeditor_ai PUBLIC VIDEOEDITOR_HAVE_ORT=0)
  endif()
endif()
