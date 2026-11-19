cmake_minimum_required(VERSION 3.25)
project(VideoCompressionWorkbench VERSION 1.2.7 LANGUAGES CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(ffmpegui_core
    src/core/process.cpp
    src/core/ssh.cpp
    src/core/conversion.cpp
    src/core/archive.cpp
    src/core/preview.cpp
    src/core/app_controller.cpp
)
target_include_directories(ffmpegui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(WIN32)
    target_compile_definitions(ffmpegui_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
find_package(LibArchive REQUIRED)
find_package(Threads REQUIRED)
target_link_libraries(ffmpegui_core PUBLIC LibArchive::LibArchive Threads::Threads)

if(MSVC)
    target_compile_options(ffmpegui_core PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(ffmpegui_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    set(APP_ICON_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/video-compression-workbench.ico")
    set(APP_MANIFEST_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows_app.manifest")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows_app.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/windows_app.rc
        @ONLY
    )
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows_version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/windows_version.rc
        @ONLY
    )
    set_source_files_properties(
        ${CMAKE_CURRENT_BINARY_DIR}/windows_app.rc
        PROPERTIES OBJECT_DEPENDS "${APP_ICON_SOURCE}"
    )
    add_executable(ffmpeg-ui WIN32
        src/main.cpp
        src/platform/windows_ui.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/windows_app.rc
        ${CMAKE_CURRENT_BINARY_DIR}/windows_version.rc
    )
    target_include_directories(ffmpeg-ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(ffmpeg-ui PRIVATE ffmpegui_core user32 gdi32 comdlg32 shell32 ole32 dwmapi uxtheme comctl32 advapi32)
    target_compile_definitions(ffmpeg-ui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    if(MSVC)
        target_link_options(ffmpeg-ui PRIVATE /MANIFEST:NO)
    endif()
else()
    find_package(X11 REQUIRED)
    find_library(XFT_LIBRARY NAMES Xft libXft.so.2 REQUIRED)
    add_executable(ffmpeg-ui
        src/main.cpp
        src/platform/linux_ui.cpp
    )
    target_link_libraries(ffmpeg-ui PRIVATE ffmpegui_core X11::X11 ${XFT_LIBRARY})
endif()

if(MSVC)
    target_compile_options(ffmpeg-ui PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(ffmpeg-ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(ffmpegui-core-tests tests/core_tests.cpp)
    target_link_libraries(ffmpegui-core-tests PRIVATE ffmpegui_core)
    target_include_directories(ffmpegui-core-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(ffmpegui-core-tests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME core-tests COMMAND ffmpegui-core-tests)
endif()

set_target_properties(ffmpeg-ui PROPERTIES OUTPUT_NAME "video-compression-workbench")

install(TARGETS ffmpeg-ui RUNTIME DESTINATION bin)
