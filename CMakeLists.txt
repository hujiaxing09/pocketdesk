cmake_minimum_required(VERSION 3.25)
project(pocketdesk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 静态链接 CRT，与 vcpkg 的 x64-windows-static triplet 一致
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(OpenSSL REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)
find_package(unofficial-nayuki-qr-code-generator CONFIG REQUIRED)

# WIN32：托盘程序，没有控制台窗口；日志写到 %LOCALAPPDATA%\PocketDesk\logs
add_executable(pocketdesk WIN32
    res/pocketdesk.rc
    src/main.cpp
    src/cursor.cpp
    src/devices.cpp
    src/input.cpp
    src/tray.cpp
    src/log.cpp
    src/certs.cpp
    src/capture.cpp
    src/encoder.cpp
    src/streamer.cpp
    src/web_server.cpp
    src/netinfo.cpp
)

target_compile_definitions(pocketdesk PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006 _CRT_SECURE_NO_WARNINGS
    CPPHTTPLIB_OPENSSL_SUPPORT)
target_compile_options(pocketdesk PRIVATE /utf-8 /W3 /permissive- /EHsc)
target_include_directories(pocketdesk PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(pocketdesk PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(pocketdesk PRIVATE
    ${FFMPEG_LIBRARIES}
    LibDataChannel::LibDataChannel
    httplib::httplib
    nlohmann_json::nlohmann_json
    unofficial::nayuki-qr-code-generator::nayuki-qr-code-generator
    OpenSSL::SSL OpenSSL::Crypto
    d3d11 dxgi d3dcompiler winmm iphlpapi ws2_32 shell32)

# 网页文件复制到 exe 旁边的 web/ 目录
add_custom_command(TARGET pocketdesk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web $<TARGET_FILE_DIR:pocketdesk>/web)
