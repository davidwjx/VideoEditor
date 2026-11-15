add_library(videoeditor_app STATIC
  AppController.h
  AppController.cpp
)
target_include_directories(videoeditor_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(videoeditor_app PUBLIC
  videoeditor_core videoeditor_media videoeditor_ui videoeditor_ai
  Qt6::Core Qt6::Network)
# The command-line timeline self-tests build their project from the checked-in
# fixtures; tests/CMakeLists.txt invokes the same binary from the build tree.
target_compile_definitions(videoeditor_app PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")

qt_add_executable(VideoEditor main.cpp)
qt_add_qml_module(VideoEditor
  URI VideoEditor
  VERSION 1.0
  QML_FILES
    Main.qml
    TimelinePanel.qml
    TimelineClipDelegate.qml
    TimelineCueDelegate.qml
    TrackHeaderDelegate.qml
    InspectorPanel.qml
    SubtitlePanel.qml
    RemovalPanel.qml
    ConfidenceBand.qml
    JobProgressPanel.qml
    MaskEditorOverlay.qml
    ExportDialog.qml
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

# The UI launches the inference engine as a sibling process. ONNX Runtime and
# DirectML DLLs are deliberately not bundled until a signed model release
# exists; copy them only if a future build places them in the tree.
if(TARGET VisionAiEngine)
  add_custom_command(TARGET VideoEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:VisionAiEngine>" "$<TARGET_FILE_DIR:VideoEditor>")
endif()
videoeditor_copy_ai_runtime(VideoEditor)
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/models")
  # Refresh every build, not only when VideoEditor relinks, so the test
  # executables in the same directory always see the current manifest.
  add_custom_target(videoeditor_models_copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/models"
            "$<TARGET_FILE_DIR:VideoEditor>/models"
    COMMENT "Refreshing models manifest in the build tree")
endif()
