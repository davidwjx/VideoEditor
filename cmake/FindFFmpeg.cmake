set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg prefix")
if(NOT FFMPEG_ROOT AND DEFINED VCPKG_INSTALLED_DIR)
  set(FFMPEG_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h HINTS "${FFMPEG_ROOT}/include" REQUIRED)

set(_ffmpeg_libs avcodec avformat avutil swscale swresample)
foreach(_lib IN LISTS _ffmpeg_libs)
  find_library(FFMPEG_${_lib}_LIBRARY_RELEASE NAMES ${_lib} HINTS "${FFMPEG_ROOT}/lib")
  find_library(FFMPEG_${_lib}_LIBRARY_DEBUG NAMES ${_lib} HINTS "${FFMPEG_ROOT}/debug/lib")
  if(NOT FFMPEG_${_lib}_LIBRARY_RELEASE)
    message(FATAL_ERROR "Missing FFmpeg library: ${_lib}")
  endif()
  # The BtbN LGPL build is release-only (no debug/lib); fall back to release.
  if(NOT FFMPEG_${_lib}_LIBRARY_DEBUG)
    set(FFMPEG_${_lib}_LIBRARY_DEBUG "${FFMPEG_${_lib}_LIBRARY_RELEASE}")
  endif()
  if(NOT TARGET FFmpeg::${_lib})
    add_library(FFmpeg::${_lib} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${_lib} PROPERTIES
      IMPORTED_LOCATION "${FFMPEG_${_lib}_LIBRARY_RELEASE}"
      IMPORTED_LOCATION_RELEASE "${FFMPEG_${_lib}_LIBRARY_RELEASE}"
      IMPORTED_LOCATION_DEBUG "${FFMPEG_${_lib}_LIBRARY_DEBUG}"
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
  endif()
endforeach()

set(FFmpeg_FOUND TRUE)
