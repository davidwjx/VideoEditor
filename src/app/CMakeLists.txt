qt_add_executable(VideoEditor main.cpp)
qt_add_qml_module(VideoEditor
  URI VideoEditor
  VERSION 1.0
  QML_FILES Main.qml
)
target_link_libraries(VideoEditor PRIVATE Qt6::Quick Qt6::QuickControls2 videoeditor_core)
