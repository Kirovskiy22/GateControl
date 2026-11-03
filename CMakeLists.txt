cmake_minimum_required(VERSION 3.18)
project(GateControlCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    add_compile_options(/utf-8 /O2 /W3)
else()
    add_compile_options(-O3 -Wall -pthread)
endif()

include(FetchContent)

# 1. JSON (nlohmann/json)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# 2. HTTP Server & Client (cpp-httplib)
FetchContent_Declare(
    httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.15.3.tar.gz
)
FetchContent_MakeAvailable(httplib)

# 3. Нативное окно Windows (webview / WebView2)
FetchContent_Declare(
    webview
    URL https://github.com/webview/webview/archive/refs/tags/0.12.0.tar.gz
)
FetchContent_MakeAvailable(webview)

# 4. OpenCV и ONNX Runtime
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)

set(ONNXRUNTIME_ROOTDIR "C:/onnxruntime" CACHE PATH "Path to ONNX Runtime")
include_directories(
    ${OpenCV_INCLUDE_DIRS}
    "${ONNXRUNTIME_ROOTDIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
link_directories("${ONNXRUNTIME_ROOTDIR}/lib")

# Флаг WIN32 создает оконное приложение без черной консоли
add_executable(GateControl WIN32 src/main.cpp)

target_link_libraries(GateControl PRIVATE
    ${OpenCV_LIBS}
    onnxruntime
    nlohmann_json::nlohmann_json
    httplib::httplib
    webview::core_static
)

if(WIN32)
    target_link_libraries(GateControl PRIVATE ws2_32 crypt32 shell32 ole32 user32)
    if(MSVC)
        set_target_properties(GateControl PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    endif()
endif()