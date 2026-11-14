find_package(FFmpeg REQUIRED)

add_library(videoeditor_media STATIC
  probe/MediaProbe.h
  probe/MediaProbe.cpp
  probe/FfmpegError.h
  probe/FfmpegError.cpp
  decode/D3D11Context.h
  decode/D3D11Context.cpp
  decode/SharedTextureRing.h
  decode/SharedTextureRing.cpp
  decode/Shaders.h
  decode/Nv12Converter.h
  decode/Nv12Converter.cpp
  decode/RgbaConverter.h
  decode/RgbaConverter.cpp
  decode/FrameReadback.h
  decode/FrameReadback.cpp
  decode/IVideoDecoder.h
  decode/IVideoDecoder.cpp
  decode/FfmpegD3D11Decoder.h
  decode/FfmpegD3D11Decoder.cpp
  decode/FfmpegSoftwareDecoder.h
  decode/FfmpegSoftwareDecoder.cpp
  decode/VideoDecoderFactory.h
  decode/VideoDecoderFactory.cpp
  decode/FramePublisher.h
  decode/FramePublisher.cpp
)
# Accept both "probe/MediaProbe.h" and "MediaProbe.h" from consumers, and the
# same for the decode headers.
target_include_directories(videoeditor_media PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/probe
  ${CMAKE_CURRENT_SOURCE_DIR}/decode)
target_link_libraries(videoeditor_media
  PUBLIC videoeditor_core Qt6::Core Qt6::Gui
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
          d3d11 dxgi d3dcompiler
)
target_compile_definitions(videoeditor_media PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
