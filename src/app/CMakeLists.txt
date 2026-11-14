add_library(videoeditor_app STATIC
  AppController.h
  AppController.cpp
)
target_include_directories(videoeditor_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_app PUBLIC videoeditor_core videoeditor_media videoeditor_ui Qt6::Core)

qt_add_executable(VideoEditor main.cpp)
qt_add_qml_module(VideoEditor
  URI VideoEditor
  VERSION 1.0
  QML_FILES Main.qml
)
target_link_libraries(VideoEditor PRIVATE Qt6::Quick Qt6::QuickControls2 videoeditor_app)

if(WIN32 AND FFMPEG_ROOT)
  file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  # Proxy generation shells out to the FFmpeg CLI, so it has to ship with the
  # app next to the libraries it loads.
  set(FFMPEG_RUNTIME_FILES ${FFMPEG_DLLS})
  if(EXISTS "${FFMPEG_ROOT}/bin/ffmpeg.exe")
    list(APPEND FFMPEG_RUNTIME_FILES "${FFMPEG_ROOT}/bin/ffmpeg.exe")
  endif()
  add_custom_command(TARGET VideoEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFMPEG_RUNTIME_FILES} "$<TARGET_FILE_DIR:VideoEditor>")
endif()
