cmake_minimum_required(VERSION 3.22)
project(vsp_backend LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0>=1.20
  gstreamer-app-1.0
  gstreamer-pbutils-1.0
  gstreamer-video-1.0
  gstreamer-audio-1.0)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
find_package(Boost 1.78 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

add_library(vsp_core STATIC
  src/core/config.cpp
  src/core/util.cpp
  src/storage/database.cpp
  src/media/gst_init.cpp
  src/media/mxf_fixup.cpp
  src/media/encoder_detect.cpp
  src/media/probe.cpp
  src/media/ladder.cpp
  src/media/mp4_box.cpp
  src/media/cmaf_packager.cpp
  src/media/transcoder.cpp
  src/media/thumbnail.cpp
  src/jobs/job_queue.cpp
  src/http/events.cpp
  src/http/router.cpp
  src/http/static_files.cpp
  src/http/api.cpp
  src/http/server.cpp
  src/http/playlist_registry.cpp
  src/live/rtmp/amf0.cpp
  src/live/rtmp/chunk.cpp
  src/live/rtmp/session.cpp
  src/live/rtmp/server.cpp
  src/live/live_pipeline.cpp
  src/live/live_manager.cpp)

target_include_directories(vsp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(vsp_core PUBLIC
  PkgConfig::GST
  PkgConfig::SQLITE3
  Boost::headers
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  Threads::Threads)
target_compile_definitions(vsp_core PUBLIC
  BOOST_ASIO_NO_DEPRECATED
  VSP_VERSION="${PROJECT_VERSION}")
target_compile_options(vsp_core PUBLIC -Wall -Wextra -Wno-unused-parameter)

add_executable(vsp-server src/main.cpp)
target_link_libraries(vsp-server PRIVATE vsp_core)

add_executable(vsp-probe tools/probe_main.cpp)
target_link_libraries(vsp-probe PRIVATE vsp_core)

add_executable(vsp-mp4-tests tests/mp4_box_tests.cpp)
target_link_libraries(vsp-mp4-tests PRIVATE vsp_core)
add_executable(vsp-rtmp-tests tests/rtmp_tests.cpp)
target_link_libraries(vsp-rtmp-tests PRIVATE vsp_core)
enable_testing()
add_test(NAME mp4_box_tests COMMAND vsp-mp4-tests)
add_test(NAME rtmp_tests COMMAND vsp-rtmp-tests)

install(TARGETS vsp-server vsp-probe RUNTIME DESTINATION bin)
