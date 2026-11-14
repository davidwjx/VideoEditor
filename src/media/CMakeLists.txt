find_package(FFmpeg REQUIRED)

add_library(videoeditor_media STATIC
  probe/MediaProbe.h
  probe/MediaProbe.cpp
  probe/FfmpegError.h
  probe/FfmpegError.cpp
)
# Accept both "probe/MediaProbe.h" and "MediaProbe.h" from consumers.
target_include_directories(videoeditor_media PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/probe)
target_link_libraries(videoeditor_media
  PUBLIC videoeditor_core Qt6::Core
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
)
target_compile_definitions(videoeditor_media PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
