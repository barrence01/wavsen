
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAVSEN_AVFORMAT    REQUIRED IMPORTED_TARGET libavformat>=58)
pkg_check_modules(WAVSEN_AVCODEC     REQUIRED IMPORTED_TARGET libavcodec>=58)
pkg_check_modules(WAVSEN_AVUTIL      REQUIRED IMPORTED_TARGET libavutil>=56)
pkg_check_modules(WAVSEN_SWSCALE     REQUIRED IMPORTED_TARGET libswscale>=5)
pkg_check_modules(WAVSEN_SWRESAMPLE           IMPORTED_TARGET libswresample>=3)
# Optional: VAAPI hardware decode. Headers + symbols come from libavutil
# itself; libva pkg-config is presence-checked so the build also succeeds
# against an FFmpeg compiled without --enable-vaapi.
pkg_check_modules(WAVSEN_LIBVA                IMPORTED_TARGET libva libva-drm)

if(APPLE)
    enable_language(OBJCXX)
endif()

if(APPLE)
    set(_wavsen_default_audio_backend "coreaudio")
else()
    set(_wavsen_default_audio_backend "pulse")
endif()
set(WAVSEN_AUDIO_BACKEND "${_wavsen_default_audio_backend}" CACHE STRING
    "Audio backend for wavsen::audio (pulse|pipewire|coreaudio)")
set_property(CACHE WAVSEN_AUDIO_BACKEND PROPERTY STRINGS pulse pipewire coreaudio)

if(WAVSEN_AUDIO_BACKEND STREQUAL "coreaudio")
    if(NOT APPLE)
        message(FATAL_ERROR "WAVSEN_AUDIO_BACKEND=coreaudio is only available on Apple platforms")
    endif()
elseif(WAVSEN_AUDIO_BACKEND STREQUAL "pulse")
    pkg_check_modules(WAVSEN_LIBPULSE REQUIRED libpulse>=14.0)
elseif(WAVSEN_AUDIO_BACKEND STREQUAL "pipewire")
    pkg_check_modules(WAVSEN_LIBPIPEWIRE REQUIRED libpipewire-0.3)
else()
    message(FATAL_ERROR
        "WAVSEN_AUDIO_BACKEND='${WAVSEN_AUDIO_BACKEND}' is not one of: pulse pipewire coreaudio")
endif()

if(APPLE)
    # Keep the Apple-only Objective-C++ implementation behind small C ABI
    # bridge libraries. The module-bearing wavsen targets remain C++20-only.
    add_subdirectory(src/video)
    if(WAVSEN_AUDIO_BACKEND STREQUAL "coreaudio")
        add_subdirectory(src/audio)
    endif()
endif()

if(NOT TARGET vvk::vvk)
    message(FATAL_ERROR
        "wavsen requires the host project to provide vvk::vvk")
endif()

# ---------------------------------------------------------------------------
# wavsen::ffi::* — C++20 module wrappers around external APIs. Each is
# its own STATIC library so external consumers can pick & link them.
# ---------------------------------------------------------------------------
add_subdirectory(src/ffi/ffmpeg)
if(WAVSEN_AUDIO_BACKEND STREQUAL "coreaudio")
    # CoreAudio is a system framework backend and does not need a wavsen FFI
    # wrapper. The source is Objective-C++ because AudioQueue types are C APIs
    # exposed by the Apple SDK and the host may also provide ObjC runtime flags.
    set(_wavsen_audio_backend_target "")
elseif(WAVSEN_AUDIO_BACKEND STREQUAL "pulse")
    add_subdirectory(src/ffi/pulse)
else()
    add_subdirectory(src/ffi/pipewire)
endif()

add_library(wavsen_decode STATIC)
add_library(wavsen::decode ALIAS wavsen_decode)

target_compile_features(wavsen_decode PUBLIC cxx_std_20)

target_sources(wavsen_decode
    PUBLIC FILE_SET CXX_MODULES FILES
        src/decode/mod.cppm
)
target_sources(wavsen_decode PRIVATE
    src/decode/mod.cpp
)

target_link_libraries(wavsen_decode
    PUBLIC
        rstd::rstd
        wavsen::ffi::ffmpeg
    PRIVATE
        rstd.cppstd
)
set_target_properties(wavsen_decode PROPERTIES CXX_SCAN_FOR_MODULES ON)


add_subdirectory(shaders)

add_library(wavsen_video STATIC)
add_library(wavsen::video ALIAS wavsen_video)

target_compile_features(wavsen_video PUBLIC cxx_std_20)
target_sources(wavsen_video
    PUBLIC FILE_SET CXX_MODULES FILES
        src/video/mod.cppm
        src/video/vk_device.cppm
        src/video/video_decoder.cppm
        src/video/yuv_to_rgba.cppm
        src/video/presenter.cppm
)
target_sources(wavsen_video PRIVATE
    src/video/vk_device.cpp
    src/video/video_decoder.cpp
    src/video/yuv_to_rgba.cpp
)
target_link_libraries(wavsen_video
    PUBLIC
        rstd::rstd
        wavsen::ffi::ffmpeg
        vvk::vvk
    PRIVATE
        rstd.log
        WavsenShader::nv12_to_rgba
)
if(APPLE)
    target_link_libraries(wavsen_video PRIVATE WavsenAppleBridge::bridge)
endif()
set_property(TARGET wavsen_video PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(wavsen_video PROPERTIES CXX_SCAN_FOR_MODULES ON)

if(NOT WAVSEN_SWRESAMPLE_FOUND)
    message(FATAL_ERROR
        "wavsen::audio requires libswresample (pkg-config). Install "
        "ffmpeg dev headers including libswresample.")
endif()

add_library(wavsen_audio STATIC)
add_library(wavsen::audio ALIAS wavsen_audio)

target_compile_features(wavsen_audio PUBLIC cxx_std_20)

target_sources(wavsen_audio
    PUBLIC FILE_SET CXX_MODULES FILES
        src/audio/byte_stream.cppm
        src/audio/core.cppm
        src/audio/mixer.cppm
        src/audio/file.cppm
        src/audio/av_sync.cppm
        src/audio/capture.cppm
        src/audio/mod.cppm
)
target_sources(wavsen_audio
    PRIVATE FILE_SET wavsen_audio_internal_modules TYPE CXX_MODULES FILES
        src/audio/gain.cppm
        src/audio/capture_window.cppm
)
target_sources(wavsen_audio PRIVATE
    src/audio/file.cpp
    src/audio/mixer.cpp
    src/audio/av_sync.cpp
)

if(WAVSEN_AUDIO_BACKEND STREQUAL "coreaudio")
    target_sources(wavsen_audio PRIVATE
        src/audio/mod.cpp
        src/audio/audio_capture_coreaudio.cpp
    )
elseif(WAVSEN_AUDIO_BACKEND STREQUAL "pulse")
    target_sources(wavsen_audio PRIVATE
        src/audio/mod.cpp
        src/audio/capture.cpp
    )
    set(_wavsen_audio_backend_target wavsen::ffi::pulse)
else()
    target_sources(wavsen_audio PRIVATE
        src/audio/audio_device_pipewire.cpp
        src/audio/audio_capture_pipewire.cpp
    )
    set(_wavsen_audio_backend_target wavsen::ffi::pipewire)
endif()

target_link_libraries(wavsen_audio
    PUBLIC
        rstd::rstd
        rstd.cppstd
        wavsen::ffi::ffmpeg
    PRIVATE
        rstd.log
        ${_wavsen_audio_backend_target}
)
if(WAVSEN_AUDIO_BACKEND STREQUAL "coreaudio")
    target_link_libraries(wavsen_audio PRIVATE
        WavsenAppleAudio::capture
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    )
endif()
set_target_properties(wavsen_audio PROPERTIES CXX_SCAN_FOR_MODULES ON)

option(WAVSEN_BUILD_TESTS "Build wavsen test executables" OFF)
if(WAVSEN_BUILD_TESTS)
    enable_testing()
    add_executable(wavsen_audio_capture_window_tests
        tests/audio_capture_window_tests.cpp
    )
    target_compile_features(wavsen_audio_capture_window_tests PRIVATE cxx_std_20)
    target_sources(wavsen_audio_capture_window_tests
        PRIVATE FILE_SET CXX_MODULES FILES
            src/audio/capture.cppm
            src/audio/capture_window.cppm
    )
    target_link_libraries(wavsen_audio_capture_window_tests PRIVATE rstd::rstd)
    add_test(NAME wavsen_audio_capture_window_tests
             COMMAND wavsen_audio_capture_window_tests)

    add_executable(wavsen_audio_gain_tests
        tests/audio_gain_tests.cpp
    )
    target_compile_features(wavsen_audio_gain_tests PRIVATE cxx_std_20)
    target_sources(wavsen_audio_gain_tests
        PRIVATE FILE_SET CXX_MODULES FILES
            src/audio/gain.cppm
    )
    target_link_libraries(wavsen_audio_gain_tests PRIVATE rstd::rstd)
    add_test(NAME wavsen_audio_gain_tests
             COMMAND wavsen_audio_gain_tests)

    add_executable(wavsen_presenter_tests
        tests/presenter_tests.cpp
    )
    target_compile_features(wavsen_presenter_tests PRIVATE cxx_std_20)
    target_link_libraries(wavsen_presenter_tests PRIVATE wavsen::video)
    add_test(NAME wavsen_presenter_tests
             COMMAND wavsen_presenter_tests)

    add_executable(wavsen_stream_decoder_rate_tests
        tests/stream_decoder_rate_tests.cpp
    )
    target_compile_features(wavsen_stream_decoder_rate_tests PRIVATE cxx_std_20)
    target_link_libraries(wavsen_stream_decoder_rate_tests PRIVATE wavsen::audio)
    add_test(NAME wavsen_stream_decoder_rate_tests
             COMMAND wavsen_stream_decoder_rate_tests)
endif()
