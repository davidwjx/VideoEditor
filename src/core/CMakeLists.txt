add_library(videoeditor_core STATIC
  AppInfo.h
  AppInfo.cpp
  TimeBase.h
  TimeBase.cpp
  MediaInfo.h
  CodecSupport.h
  CodecSupport.cpp
  Project.h
  ProjectSerializer.h
  ProjectSerializer.cpp
  ProjectStore.h
  ProjectStore.cpp
  ProjectDocument.h
  ProjectDocument.cpp
  commands/AddAssetCommand.h
  commands/AddAssetCommand.cpp
  commands/RemoveAssetCommand.h
  commands/RemoveAssetCommand.cpp
)
target_include_directories(videoeditor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_core PUBLIC Qt6::Core Qt6::Gui)
