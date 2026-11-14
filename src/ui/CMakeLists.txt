add_library(videoeditor_ui STATIC
  MediaListModel.h
  MediaListModel.cpp
  FrameBridge.h
  FrameBridge.cpp
  VideoPreviewItem.h
  VideoPreviewItem.cpp
  DecoderWorker.h
  DecoderWorker.cpp
  PreviewController.h
  PreviewController.cpp
)
target_include_directories(videoeditor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ui
  PUBLIC videoeditor_core videoeditor_media Qt6::Core Qt6::Gui Qt6::Quick
  PRIVATE d3d11 dxgi
)
