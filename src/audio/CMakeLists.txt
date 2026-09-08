cmake_minimum_required(VERSION 3.28)

if(DEFINED CMAKE_CXX_USING_LINKER_lito_configured AND
   NOT DEFINED CMAKE_OBJCXX_USING_LINKER_lito_configured)
    set(CMAKE_OBJCXX_USING_LINKER_lito_configured
        "${CMAKE_CXX_USING_LINKER_lito_configured}"
        CACHE STRING "Linker flags for the lito-configured Objective-C++ compiler")
endif()

project(WavsenAppleAudio VERSION 0.1.0 LANGUAGES CXX OBJCXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

add_library(wavsen_apple_audio STATIC
    apple_audio_capture_bridge.mm
    apple_audio_playback_bridge.mm
)
add_library(WavsenAppleAudio::capture ALIAS wavsen_apple_audio)
set_target_properties(wavsen_apple_audio PROPERTIES EXPORT_NAME capture)

target_compile_features(wavsen_apple_audio PUBLIC cxx_std_20)
target_include_directories(wavsen_apple_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/wavsen>
)
target_link_libraries(wavsen_apple_audio
    PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Foundation"
)

install(FILES
    apple_audio_capture_bridge.hpp
    apple_audio_playback_bridge.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wavsen)
install(TARGETS wavsen_apple_audio
    EXPORT WavsenAppleAudioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT WavsenAppleAudioTargets
    FILE WavsenAppleAudioTargets.cmake
    NAMESPACE WavsenAppleAudio::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleAudio)

configure_package_config_file(
    cmake/WavsenAppleAudioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleAudioConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleAudio)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleAudioConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleAudioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleAudioConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleAudio)
