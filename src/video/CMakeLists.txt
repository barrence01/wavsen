cmake_minimum_required(VERSION 3.28)

if(DEFINED CMAKE_CXX_USING_LINKER_lito_configured AND
   NOT DEFINED CMAKE_OBJCXX_USING_LINKER_lito_configured)
    set(CMAKE_OBJCXX_USING_LINKER_lito_configured
        "${CMAKE_CXX_USING_LINKER_lito_configured}"
        CACHE STRING "Linker flags for the lito-configured Objective-C++ compiler")
endif()

project(WavsenAppleBridge VERSION 0.1.0 LANGUAGES CXX OBJCXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAVSEN_AVUTIL REQUIRED IMPORTED_TARGET libavutil>=56)

add_library(wavsen_apple_bridge STATIC apple_video_bridge.mm)
add_library(WavsenAppleBridge::bridge ALIAS wavsen_apple_bridge)
set_target_properties(wavsen_apple_bridge PROPERTIES EXPORT_NAME bridge)

target_compile_features(wavsen_apple_bridge PUBLIC cxx_std_20)
target_include_directories(wavsen_apple_bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/wavsen>
)
target_link_libraries(wavsen_apple_bridge
    PUBLIC
        PkgConfig::WAVSEN_AVUTIL
    PRIVATE
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework IOSurface"
        "-framework Metal"
)

install(FILES apple_video_bridge.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/wavsen)
install(TARGETS wavsen_apple_bridge
    EXPORT WavsenAppleBridgeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT WavsenAppleBridgeTargets
    FILE WavsenAppleBridgeTargets.cmake
    NAMESPACE WavsenAppleBridge::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleBridge)

configure_package_config_file(
    cmake/WavsenAppleBridgeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleBridgeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleBridge)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleBridgeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleBridgeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WavsenAppleBridgeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WavsenAppleBridge)
