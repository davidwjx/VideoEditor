include(GoogleTest)

set(TEST_ENV_MOD
  "PATH=path_list_prepend:C:/Qt/6.8.3/msvc2022_64/bin"
  "PATH=path_list_prepend:${FFMPEG_ROOT}/bin")

add_executable(app_info_test core/AppInfoTest.cpp)
target_link_libraries(app_info_test PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(app_info_test PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(core_tests
  core/AppInfoTest.cpp
  core/TimeBaseTest.cpp
  core/CodecSupportTest.cpp
  core/MediaInfoTest.cpp
  core/TimelineTest.cpp
  core/TimelineEvaluatorTest.cpp
  core/ProjectSerializerTest.cpp
  core/ProjectDocumentTest.cpp
  core/TimelineEditorTest.cpp
  core/SrtTest.cpp
  core/SubtitleEditorTest.cpp
  core/ExportSettingsTest.cpp
)
target_link_libraries(core_tests PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(core_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(media_tests
  media/MediaProbeTest.cpp
  media/AudioProbeTest.cpp
  media/D3D11PipelineTest.cpp
  media/DecoderTest.cpp
  media/ProxyTest.cpp
  media/AudioSpeedTest.cpp
  media/AudioMixTest.cpp
  media/AudioSyncTest.cpp
  media/AudioEngineTest.cpp
  media/WaveformTest.cpp
  media/ExportEncoderTest.cpp
  media/VideoEncoderTest.cpp
  TestMain.cpp)
target_link_libraries(media_tests PRIVATE
  videoeditor_media GTest::gtest Qt6::Core Qt6::Gui Qt6::Test d3d11 dxgi d3dcompiler)
target_compile_definitions(media_tests PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(media_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(ui_tests
  ui/TimelineModelTest.cpp
  ui/TimelineControllerTest.cpp
  ui/WaveformImageProviderTest.cpp
  TestGuiMain.cpp)
target_link_libraries(ui_tests PRIVATE
  videoeditor_ui GTest::gtest Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(ui_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(ui_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

# Render pipeline tests need a GUI application (QPainter text uses the font
# database). Like ui_tests they use the default platform: the Windows offscreen
# plugin hangs QGuiApplication startup, while no window is actually shown.
add_executable(render_tests
  media/FrameSelectorTest.cpp
  media/SubtitleRasterizerTest.cpp
  media/CompositorTest.cpp
  media/ClipVideoSourceTest.cpp
  media/TimelineCoalescerTest.cpp
  media/SourceCachePolicyTest.cpp
  media/OfflineRendererTest.cpp
  TestGuiMain.cpp)
target_link_libraries(render_tests PRIVATE
  videoeditor_media GTest::gtest Qt6::Core Qt6::Gui d3d11 dxgi d3dcompiler)
target_compile_definitions(render_tests PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(render_tests PROPERTIES
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(app_tests
  app/AppControllerTest.cpp
  app/TimelineUiTest.cpp
  app/TimelineIntegrationTest.cpp
  TestMain.cpp)
target_link_libraries(app_tests PRIVATE videoeditor_app GTest::gtest Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(app_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(app_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

# Loads Main.qml from the source tree and drives a real click/drag through the
# Qt Quick event delivery path, so Flickable/MouseArea interaction regressions
# (such as a stolen drag) fail here instead of only in manual UAT.
add_executable(app_qml_tests app/TimelineQmlTest.cpp TestGuiMain.cpp)
target_link_libraries(app_qml_tests PRIVATE
  videoeditor_app GTest::gtest
  Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Qml Qt6::Test)
target_compile_definitions(app_qml_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
  VIDEOEDITOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME TimelineQml
         COMMAND app_qml_tests --gtest_filter=TimelineQml.*)
set_tests_properties(TimelineQml PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  LABELS "gpu"
  TIMEOUT 60)

add_test(NAME preview_selftest
         COMMAND VideoEditor --selftest "${CMAKE_SOURCE_DIR}/tests/fixtures/h264.mp4")
set_tests_properties(preview_selftest PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 30
  LABELS "gpu"
)

# One end-to-end preview selftest per supported fixture. Each test opens the
# fixture in the Qt Quick preview window and exits after the first presented
# frame, so it needs an interactive desktop session.
foreach(_fixture IN ITEMS
    mpeg4.mp4 h264.mp4 h265.mp4 h265_10bit.mp4 wmv2.wmv rotated.mp4
    "中文 文件名.mp4" noaudio.mp4)
  get_filename_component(_name "${_fixture}" NAME_WE)
  add_test(NAME "preview_${_name}"
           COMMAND VideoEditor --selftest
                   "${CMAKE_SOURCE_DIR}/tests/fixtures/${_fixture}")
  set_tests_properties("preview_${_name}" PROPERTIES
    ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
    ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
    TIMEOUT 30
    LABELS "gpu"
  )
endforeach()

# Multi-track timeline self-test: 3 video layers, 2 audio tracks and a subtitle
# track composited at 1920x1080. Exits 0 after 30 presented frames, 2 on the
# internal 15 s timeout.
add_test(NAME timeline_selftest
         COMMAND VideoEditor --selftest-timeline)
set_tests_properties(timeline_selftest PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 30
  LABELS "gpu")

# Performance gate (Release only): the same project must present at least
# 24 fps over five seconds. The app exits 3 when the threshold is missed.
add_test(NAME timeline_perf
         COMMAND VideoEditor --selftest-perf)
set_tests_properties(timeline_perf PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 60
  CONFIGURATIONS Release
  LABELS "gpu;performance")
