cmake_minimum_required(VERSION 3.24)

project(EECampEduInputDemo CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL3 CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})

add_executable(eecampedu_input_demo
    src/main.cpp
    src/App/App.cpp
    src/App/AppState.cpp
    src/Windows/CameraViewerWin.cpp
    src/Windows/ControlsWin.cpp
    src/Usb/UsbCdcClient.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(eecampedu_input_demo PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_link_libraries(eecampedu_input_demo PRIVATE SDL3::SDL3)

if (WIN32)
    target_compile_definitions(eecampedu_input_demo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
