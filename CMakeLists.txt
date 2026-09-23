cmake_minimum_required(VERSION 3.24)
project(prores_gpu_lab LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(GSTREAMER_ROOT "C:/Program Files/gstreamer/1.0/msvc_x86_64" CACHE PATH "GStreamer MSVC x64 SDK")
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/tools/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1" CACHE PATH "FFmpeg shared SDK")
add_executable(prores_bench src/bench.cpp)
target_include_directories(prores_bench PRIVATE "${FFMPEG_ROOT}/include" "${CMAKE_SOURCE_DIR}/external/Vulkan-Headers/include")
target_link_directories(prores_bench PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(prores_bench PRIVATE avformat avcodec avutil d3d11 dxgi d3dcompiler psapi)
target_compile_definitions(prores_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES)
if(MSVC)
  target_compile_options(prores_bench PRIVATE /W4 /utf-8)
endif()

option(BUILD_GSTREAMER_PLUGIN "GStreamerの実験用ProRes GPU要素をビルドする" ON)
if(BUILD_GSTREAMER_PLUGIN)
  if(NOT EXISTS "${GSTREAMER_ROOT}/include/gstreamer-1.0/gst/gst.h")
    message(FATAL_ERROR "GStreamer MSVC x64 SDKが必要です。GSTREAMER_ROOTを指定してください")
  endif()
  add_library(gstproresvk MODULE src/gstproresvkdec.cpp)
  target_include_directories(gstproresvk PRIVATE "${FFMPEG_ROOT}/include"
    "${CMAKE_SOURCE_DIR}/external/Vulkan-Headers/include"
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include")
  target_link_directories(gstproresvk PRIVATE "${FFMPEG_ROOT}/lib" "${GSTREAMER_ROOT}/lib")
  target_link_libraries(gstproresvk PRIVATE avcodec avutil gstvideo-1.0 gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0)
  target_compile_definitions(gstproresvk PRIVATE PACKAGE="prores-gpu-lab" NOMINMAX WIN32_LEAN_AND_MEAN VK_NO_PROTOTYPES)
  set_target_properties(gstproresvk PROPERTIES PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
  if(MSVC)
    target_compile_options(gstproresvk PRIVATE /W4 /utf-8)
  endif()
  add_executable(plugin_smoke tests/plugin_smoke.cpp)
  target_include_directories(plugin_smoke PRIVATE "${GSTREAMER_ROOT}/include/gstreamer-1.0"
    "${GSTREAMER_ROOT}/include/glib-2.0" "${GSTREAMER_ROOT}/lib/glib-2.0/include")
  target_link_directories(plugin_smoke PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(plugin_smoke PRIVATE gstapp-1.0 gstvideo-1.0 gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0)
  if(MSVC)
    target_compile_options(plugin_smoke PRIVATE /W4 /utf-8)
  endif()

  add_library(gstproresd3d11 MODULE src/gstproresd3d11dec.cpp src/gstproresd3d11rgb.cpp
    src/prores_parser.cpp
    src/prores_vld.hlsl src/prores_idct.hlsl src/prores_rgb.hlsl)
  set_source_files_properties(src/prores_vld.hlsl src/prores_idct.hlsl src/prores_rgb.hlsl
    PROPERTIES HEADER_FILE_ONLY TRUE)
  target_include_directories(gstproresd3d11 PRIVATE "${CMAKE_SOURCE_DIR}/src"
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(gstproresd3d11 PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(gstproresd3d11 PRIVATE gstd3d11-1.0 gstvideo-1.0 gstbase-1.0
    gstreamer-1.0 gobject-2.0 glib-2.0 d3d11 dxgi)
  target_compile_definitions(gstproresd3d11 PRIVATE PACKAGE="prores-gpu-lab"
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  set_target_properties(gstproresd3d11 PROPERTIES PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins")
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
      "${CMAKE_SOURCE_DIR}/src/prores_rgb.hlsl"
    COMMENT "DX11 ProRes shaderをcs_5_0 bytecodeへコンパイル")
  add_custom_command(TARGET gstproresd3d11 POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_SOURCE_DIR}/src/prores_vld.hlsl" "$<TARGET_FILE_DIR:gstproresd3d11>/prores_vld.hlsl"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_SOURCE_DIR}/src/prores_idct.hlsl" "$<TARGET_FILE_DIR:gstproresd3d11>/prores_idct.hlsl"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_SOURCE_DIR}/src/prores_rgb.hlsl" "$<TARGET_FILE_DIR:gstproresd3d11>/prores_rgb.hlsl"
    COMMENT "DX11 ProRes shaderをプラグイン出力先へ配置")
  if(MSVC)
    target_compile_options(gstproresd3d11 PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_plugin_smoke tests/d3d11_plugin_smoke.cpp src/prores_parser.cpp)
  target_include_directories(d3d11_plugin_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_plugin_smoke PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_plugin_smoke PRIVATE gstd3d11-1.0 gstapp-1.0 gstvideo-1.0
    gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0 d3d11 dxgi)
  target_compile_definitions(d3d11_plugin_smoke PRIVATE
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(d3d11_plugin_smoke PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_rgb_probe tests/d3d11_rgb_probe.cpp)
  target_include_directories(d3d11_rgb_probe PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_rgb_probe PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_rgb_probe PRIVATE gstd3d11-1.0 gstapp-1.0 gstvideo-1.0
    gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0 d3d11)
  target_compile_definitions(d3d11_rgb_probe PRIVATE
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  add_dependencies(d3d11_rgb_probe gstproresd3d11)
  if(MSVC)
    target_compile_options(d3d11_rgb_probe PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_rgb_element_smoke tests/d3d11_rgb_element_smoke.cpp)
  target_include_directories(d3d11_rgb_element_smoke PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_rgb_element_smoke PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_rgb_element_smoke PRIVATE gstd3d11-1.0 gstapp-1.0
    gstvideo-1.0 gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0 d3d11)
  target_compile_definitions(d3d11_rgb_element_smoke PRIVATE
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  add_dependencies(d3d11_rgb_element_smoke gstproresd3d11)
  if(MSVC)
    target_compile_options(d3d11_rgb_element_smoke PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_plugin_bench tests/d3d11_plugin_bench.cpp)
  target_include_directories(d3d11_plugin_bench PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_plugin_bench PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_plugin_bench PRIVATE gstd3d11-1.0 gstapp-1.0 gstvideo-1.0
    gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0 psapi)
  target_compile_definitions(d3d11_plugin_bench PRIVATE
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(d3d11_plugin_bench PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_display_bench tests/d3d11_display_bench.cpp)
  target_include_directories(d3d11_display_bench PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_display_bench PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_display_bench PRIVATE gstreamer-1.0 gobject-2.0 glib-2.0 psapi user32)
  target_compile_definitions(d3d11_display_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_compile_options(d3d11_display_bench PRIVATE /W4 /utf-8)
  endif()

  add_executable(d3d11_present_control tests/d3d11_present_control.cpp)
  target_include_directories(d3d11_present_control PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_present_control PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_present_control PRIVATE gstd3d11-1.0 gstapp-1.0 gstvideo-1.0
    gstbase-1.0 gstreamer-1.0 gobject-2.0 glib-2.0 d3d11 dxgi user32)
  target_compile_definitions(d3d11_present_control PRIVATE
    GST_USE_UNSTABLE_API NOMINMAX WIN32_LEAN_AND_MEAN)
  add_dependencies(d3d11_present_control gstproresd3d11)
  if(MSVC)
    target_compile_options(d3d11_present_control PRIVATE /W4 /utf-8)
  endif()
endif()
add_executable(dx11_primitives src/dx11_primitives.cpp)
target_link_libraries(dx11_primitives PRIVATE d3d11 d3dcompiler)
target_compile_definitions(dx11_primitives PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(dx11_primitives PRIVATE /W4 /utf-8)
endif()

# 実ProResフレームのCPU参照とDX11 VLD/逆スキャン結果を全係数比較する。
add_executable(prores_dx11_coeff tests/prores_dx11_coeff.cpp src/prores_parser.cpp)
target_include_directories(prores_dx11_coeff PRIVATE "${CMAKE_SOURCE_DIR}/src" "${FFMPEG_ROOT}/include")
target_link_directories(prores_dx11_coeff PRIVATE "${FFMPEG_ROOT}/lib")
target_link_libraries(prores_dx11_coeff PRIVATE avformat avcodec avutil d3d11 dxgi d3dcompiler)
target_compile_definitions(prores_dx11_coeff PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(prores_dx11_coeff PRIVATE /W4 /utf-8)
endif()

if(BUILD_GSTREAMER_PLUGIN)
  add_executable(d3d11_memory_probe tests/d3d11_memory_probe.cpp)
  target_include_directories(d3d11_memory_probe PRIVATE
    "${GSTREAMER_ROOT}/include/gstreamer-1.0" "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include" "${GSTREAMER_ROOT}/lib/gstreamer-1.0/include")
  target_link_directories(d3d11_memory_probe PRIVATE "${GSTREAMER_ROOT}/lib")
  target_link_libraries(d3d11_memory_probe PRIVATE gstd3d11-1.0 gstvideo-1.0 gstbase-1.0
    gstreamer-1.0 gobject-2.0 glib-2.0 d3d11 dxgi)
  target_compile_definitions(d3d11_memory_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN GST_USE_UNSTABLE_API)
  if(MSVC)
    target_compile_options(d3d11_memory_probe PRIVATE /W4 /utf-8)
  endif()
endif()
