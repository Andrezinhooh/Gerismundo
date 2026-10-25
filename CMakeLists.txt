cmake_minimum_required(VERSION 3.25)
project(NativeCall VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  add_compile_options(/utf-8 /EHsc)
endif()
option(NATIVE_CALL_BUILD_CLIENT "Build the native Windows application" ${WIN32})
option(NATIVE_CALL_BUILD_SERVER "Build the WebSocket signaling service" ON)
option(NATIVE_CALL_FETCH_DEPENDENCIES "Download network dependencies instead of using vcpkg" OFF)
option(NATIVE_CALL_USE_NICE "Use a libdatachannel build backed by libnice (advanced)" OFF)
include(CTest)
find_package(Threads REQUIRED)

if(NATIVE_CALL_BUILD_SERVER OR NATIVE_CALL_BUILD_CLIENT)
  if(NATIVE_CALL_FETCH_DEPENDENCIES)
    include(FetchContent)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(TEST_APPS OFF CACHE BOOL "Do not register unbuilt libSRTP test tools" FORCE)
    set(NO_MEDIA OFF CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(USE_NICE ${NATIVE_CALL_USE_NICE} CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    FetchContent_Declare(libdatachannel
      GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
      GIT_TAG v0.22.4 GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_Declare(json
      URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
      URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
    FetchContent_MakeAvailable(libdatachannel json)
  else()
    find_package(LibDataChannel CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
  endif()
endif()

if(NATIVE_CALL_BUILD_SERVER OR NATIVE_CALL_BUILD_CLIENT)
  add_library(native-call-transport STATIC src/transport.cpp)
  target_include_directories(native-call-transport PUBLIC include)
  target_link_libraries(native-call-transport PUBLIC
    LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json Threads::Threads)
  if(NATIVE_CALL_USE_NICE)
    target_compile_definitions(native-call-transport PRIVATE NATIVE_CALL_USE_NICE=1)
  endif()
  if(MSVC)
    target_compile_options(native-call-transport PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(native-call-transport PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
endif()

if(NATIVE_CALL_BUILD_SERVER)
  add_executable(native-call-signaling src/signaling_server.cpp)
  target_link_libraries(native-call-signaling PRIVATE
    LibDataChannel::LibDataChannel nlohmann_json::nlohmann_json Threads::Threads)
  install(TARGETS native-call-signaling RUNTIME DESTINATION bin)
endif()

if(NATIVE_CALL_BUILD_CLIENT)
  if(NOT WIN32)
    message(FATAL_ERROR "The client requires Windows; build server/tests on Linux.")
  endif()
  if(NOT MSVC)
    message(FATAL_ERROR "Use MSVC with Windows SDK 10.0.22621.0+ and C++/WinRT.")
  endif()
  find_package(Opus CONFIG REQUIRED)
  find_path(SPEEXDSP_INCLUDE_DIR speex/speex_echo.h REQUIRED)
  find_library(SPEEXDSP_LIBRARY NAMES speexdsp libspeexdsp REQUIRED)
  add_executable(native-call WIN32
    src/main.cpp src/ui.cpp src/controller.cpp src/audio.cpp
    src/capture.cpp src/video.cpp app.manifest)
  target_include_directories(native-call PRIVATE include ${SPEEXDSP_INCLUDE_DIR})
  target_compile_definitions(native-call PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  target_compile_options(native-call PRIVATE /W4 /permissive- /EHsc /utf-8 /bigobj)
  target_link_libraries(native-call PRIVATE native-call-transport LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json Opus::opus ${SPEEXDSP_LIBRARY}
    Threads::Threads windowsapp runtimeobject d3d11 dxgi dwmapi
    mfplat mf mfuuid mfreadwrite wmcodecdspuuid ole32 oleaut32 uuid
    avrt mmdevapi propsys winmm user32 gdi32 comctl32 shell32 shlwapi uxtheme)
  add_custom_command(TARGET native-call POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_SOURCE_DIR}/config.example.json $<TARGET_FILE_DIR:native-call>/config.example.json)
  install(TARGETS native-call RUNTIME DESTINATION bin)
  if(BUILD_TESTING)
    add_test(NAME windows-app-startup COMMAND native-call --smoke-test
      --smoke-report "${CMAKE_CURRENT_BINARY_DIR}/smoke-report.json")
    set_tests_properties(windows-app-startup PROPERTIES TIMEOUT 15)
    add_executable(native-call-media-tests tests/windows_media_tests.cpp src/video.cpp)
    target_include_directories(native-call-media-tests PRIVATE include)
    target_compile_definitions(native-call-media-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
      _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(native-call-media-tests PRIVATE d3d11 dxgi mfplat mf mfuuid
      mfreadwrite wmcodecdspuuid ole32 oleaut32 uuid)
    target_compile_options(native-call-media-tests PRIVATE /bigobj)
    add_test(NAME windows-media-codec COMMAND native-call-media-tests)
    set_tests_properties(windows-media-codec PROPERTIES TIMEOUT 45)
  endif()
endif()

if(BUILD_TESTING)
  add_executable(native-call-core-tests tests/core_tests.cpp)
  target_include_directories(native-call-core-tests PRIVATE include)
  target_link_libraries(native-call-core-tests PRIVATE Threads::Threads)
  add_test(NAME core COMMAND native-call-core-tests)

  if(NOT TARGET Opus::opus)
    find_package(Opus CONFIG QUIET)
  endif()
  if(NOT TARGET Opus::opus)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    find_library(OPUS_LIBRARY NAMES opus libopus.so.0)
    if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
      add_library(Opus::opus UNKNOWN IMPORTED)
      set_target_properties(Opus::opus PROPERTIES IMPORTED_LOCATION "${OPUS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}")
    endif()
  endif()
  if(TARGET Opus::opus)
    add_executable(native-call-audio-tests tests/audio_tests.cpp)
    target_include_directories(native-call-audio-tests PRIVATE include)
    target_link_libraries(native-call-audio-tests PRIVATE Opus::opus)
    add_test(NAME opus-jitter COMMAND native-call-audio-tests)
  else()
    message(STATUS "Opus headers/library unavailable: opus-jitter test omitted")
  endif()

  if(NATIVE_CALL_BUILD_SERVER)
    add_executable(native-call-transport-tests tests/transport_integration.cpp)
    target_link_libraries(native-call-transport-tests PRIVATE native-call-transport)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME webrtc-mesh COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/transport_runner.py"
        $<TARGET_FILE:native-call-signaling> $<TARGET_FILE:native-call-transport-tests>)
      set_tests_properties(webrtc-mesh PROPERTIES TIMEOUT 70)
      execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import websockets"
        RESULT_VARIABLE HAVE_PYTHON_WEBSOCKETS OUTPUT_QUIET ERROR_QUIET)
      if(HAVE_PYTHON_WEBSOCKETS EQUAL 0)
        add_test(NAME signaling COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/signaling_integration.py"
          $<TARGET_FILE:native-call-signaling>)
        set_tests_properties(signaling PROPERTIES TIMEOUT 40)
      endif()
    endif()
  endif()
endif()
