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
  core/ProjectSerializerTest.cpp
  core/ProjectDocumentTest.cpp
)
target_link_libraries(core_tests PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(core_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(media_tests
  media/MediaProbeTest.cpp
  media/D3D11PipelineTest.cpp
  media/DecoderTest.cpp
  media/ProxyTest.cpp
  TestMain.cpp)
target_link_libraries(media_tests PRIVATE
  videoeditor_media GTest::gtest Qt6::Core Qt6::Gui Qt6::Test d3d11 dxgi d3dcompiler)
target_compile_definitions(media_tests PRIVATE
  NOMINMAX WIN32_LEAN_AND_MEAN
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(media_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_executable(app_tests app/AppControllerTest.cpp TestMain.cpp)
target_link_libraries(app_tests PRIVATE videoeditor_app GTest::gtest Qt6::Core Qt6::Gui Qt6::Test)
target_compile_definitions(app_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(app_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)

add_test(NAME preview_selftest
         COMMAND VideoEditor --selftest "${CMAKE_SOURCE_DIR}/tests/fixtures/h264.mp4")
set_tests_properties(preview_selftest PROPERTIES
  ENVIRONMENT "QSG_RHI_BACKEND=d3d11"
  ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
  TIMEOUT 30
  LABELS "gpu"
)
