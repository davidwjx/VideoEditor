add_library(videoeditor_ui STATIC
  MediaListModel.h
  MediaListModel.cpp
)
target_include_directories(videoeditor_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_ui PUBLIC videoeditor_core Qt6::Core)