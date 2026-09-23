cmake_minimum_required(VERSION 3.24)
project(prores_d3d11_plugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC x64 SDK")
if(NOT EXISTS "${GSTREAMER_ROOT}/include/gstreamer-1.0/gst/gst.h")
  message(FATAL_ERROR "GStreamer MSVC x64 SDKが必要です。GSTREAMER_ROOTを指定してください")
endif()

add_library(gstproresd3d11 MODULE
  src/gstproresd3d11dec.cpp src/gstproresd3d11rgb.cpp src/prores_parser.cpp
  src/d3d11_bounded_map.hpp
  src/prores_vld.hlsl src/prores_idct.hlsl src/prores_rgb.hlsl)
set_source_files_properties(src/prores_vld.hlsl src/prores_idct.hlsl src/prores_rgb.hlsl
  PROPERTIES HEADER_FILE_ONLY TRUE)
target_include_directories(gstproresd3d11 PRIVATE "${CMAKE_SOURCE_DIR}/src"
  "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
  "${GSTREAMER_ROOT}/lib/glib-2.0/include"
  "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
target_link_directories(gstproresd3d11 PRIVATE "${GSTREAMER_ROOT}/lib")
target_link_libraries(gstproresd3d11 PRIVATE gstd3d11-1.0 gstvideo-1.0 gstbase-1.0
  gstreamer-1.0 gobject-2.0 glib-2.0 d3d11 dxgi)
target_compile_definitions(gstproresd3d11 PRIVATE PACKAGE="prores-gpu-lab"
  GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(gstproresd3d11 PROPERTIES PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
if(MSVC)
  target_compile_options(gstproresd3d11 PRIVATE /W4 /utf-8)
endif()

find_program(FXC_EXECUTABLE NAMES fxc.exe
  HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64")
if(NOT FXC_EXECUTABLE)
  message(FATAL_ERROR "Windows SDK x64 fxc.exeが必要です")
endif()
add_custom_command(TARGET gstproresd3d11 PRE_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:gstproresd3d11>"
  COMMAND "${FXC_EXECUTABLE}" /nologo /Ges /O3 /T cs_5_0 /E main
    /Fo "$<TARGET_FILE_DIR:gstproresd3d11>/prores_vld.cso"
    "${CMAKE_SOURCE_DIR}/src/prores_vld.hlsl"
  COMMAND "${FXC_EXECUTABLE}" /nologo /Ges /O3 /T cs_5_0 /E main
    /D PRORES_OUTPUT_UNORM=1
    /Fo "$<TARGET_FILE_DIR:gstproresd3d11>/prores_idct_unorm.cso"
    "${CMAKE_SOURCE_DIR}/src/prores_idct.hlsl"
  COMMAND "${FXC_EXECUTABLE}" /nologo /Ges /O3 /T cs_5_0 /E main
    /Fo "$<TARGET_FILE_DIR:gstproresd3d11>/prores_rgb.cso"
    "${CMAKE_SOURCE_DIR}/src/prores_rgb.hlsl")
