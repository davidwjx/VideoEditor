include(GoogleTest)

set(TEST_ENV_MOD
  "PATH=path_list_prepend:C:/Qt/6.8.3/msvc2022_64/bin"
  "PATH=path_list_prepend:${FFMPEG_ROOT}/bin")

add_executable(app_info_test core/AppInfoTest.cpp)
target_link_libraries(app_info_test PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(app_info_test PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)

add_executable(core_tests
  core/AppInfoTest.cpp
  core/TimeBaseTest.cpp
  core/CodecSupportTest.cpp
  core/MediaInfoTest.cpp
)
target_link_libraries(core_tests PRIVATE videoeditor_core GTest::gtest_main Qt6::Core)
gtest_discover_tests(core_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)

add_executable(media_tests media/MediaProbeTest.cpp TestMain.cpp)
target_link_libraries(media_tests PRIVATE videoeditor_media GTest::gtest Qt6::Core)
target_compile_definitions(media_tests PRIVATE
  VIDEOEDITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
gtest_discover_tests(media_tests PROPERTIES ENVIRONMENT_MODIFICATION "${TEST_ENV_MOD}"
                          DISCOVERY_MODE PRE_TEST)
