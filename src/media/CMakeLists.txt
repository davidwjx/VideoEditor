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
  proxy/AssetFingerprint.h
  proxy/AssetFingerprint.cpp
  proxy/ProxyPlanner.h
  proxy/ProxyPlanner.cpp
  proxy/ProxyGenerator.h
  proxy/ProxyGenerator.cpp
  proxy/ProxyService.h
  proxy/ProxyService.cpp
  cache/CacheStore.h
  cache/CacheStore.cpp
  audio/AudioBuffer.h
  audio/AudioClock.h
  audio/AudioSink.h
  audio/WasapiOutput.h
  audio/WasapiOutput.cpp
  audio/AudioEngine.h
  audio/AudioEngine.cpp
  audio/FfmpegAudioSource.h
  audio/FfmpegAudioSource.cpp
  audio/SpeedProcessor.h
  audio/SpeedProcessor.cpp
  audio/ClipAudioReader.h
  audio/ClipAudioReader.cpp
  audio/TimelineMixer.h
  audio/TimelineMixer.cpp
  audio/Waveform.h
  audio/Waveform.cpp
  audio/WaveformService.h
  audio/WaveformService.cpp
  render/FrameSelector.h
  render/FrameSelector.cpp
  render/FrameRequestCoalescer.h
  render/SourceCachePolicy.h
  render/SourceCachePolicy.cpp
  render/LayerGeometry.h
  render/LayerGeometry.cpp
  render/SubtitleRasterizer.h
  render/SubtitleRasterizer.cpp
  render/LayerCompositor.h
  render/LayerCompositor.cpp
  render/ClipVideoSource.h
  render/ClipVideoSource.cpp
  render/TimelineCompositor.h
  render/TimelineCompositor.cpp
  render/TimelineRenderWorker.h
  render/TimelineRenderWorker.cpp
  render/OfflineFrameRenderer.h
  render/OfflineFrameRenderer.cpp
  export/ExportEncoderCatalog.h
  export/ExportEncoderCatalog.cpp
  export/ExportPackets.h
  export/VideoEncoder.h
  export/VideoEncoder.cpp
  export/AudioEncoder.h
  export/AudioEncoder.cpp
)
# Accept both "probe/MediaProbe.h" and "MediaProbe.h" from consumers, and the
# same for the decode, proxy/cache, audio and render headers.
target_include_directories(videoeditor_media PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/probe
  ${CMAKE_CURRENT_SOURCE_DIR}/decode
  ${CMAKE_CURRENT_SOURCE_DIR}/proxy
  ${CMAKE_CURRENT_SOURCE_DIR}/cache
  ${CMAKE_CURRENT_SOURCE_DIR}/audio
  ${CMAKE_CURRENT_SOURCE_DIR}/render
  ${CMAKE_CURRENT_SOURCE_DIR}/export
  # ExportEncoderCatalog.h exposes AVPixelFormat/AVCodecContext to consumers.
  ${FFMPEG_INCLUDE_DIR})
target_link_libraries(videoeditor_media
  PUBLIC videoeditor_core Qt6::Core Qt6::Gui
  PRIVATE FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample
          d3d11 dxgi d3dcompiler ole32 uuid
)
target_compile_definitions(videoeditor_media PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
