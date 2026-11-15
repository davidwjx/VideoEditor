add_library(videoeditor_ui STATIC
  MediaListModel.h
  MediaListModel.cpp
  TimelineModel.h
  TimelineModel.cpp
  TrackListModel.h
  TrackListModel.cpp
  TimelineController.h
  TimelineController.cpp
  FrameBridge.h
  FrameBridge.cpp
  VideoPreviewItem.h
  VideoPreviewItem.cpp
  DecoderWorker.h
  DecoderWorker.cpp
  PreviewController.h
  PreviewController.cpp
  WaveformImageProvider.h
  WaveformImageProvider.cpp
  ExportController.h
  ExportController.cpp
)
target_include_directories(videoeditor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# PreviewController includes AudioEngine (std::max) together with the Windows
# headers pulled in by FrameBridge; keep the min/max macros out of the way.
target_compile_definitions(videoeditor_ui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(videoeditor_ui
  PUBLIC videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Quick
  PRIVATE d3d11 dxgi ole32
)
