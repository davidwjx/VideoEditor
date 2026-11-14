add_library(videoeditor_core STATIC
  AppInfo.h
  AppInfo.cpp
)
target_include_directories(videoeditor_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_core PUBLIC Qt6::Core)
