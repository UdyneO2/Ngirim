cmake_minimum_required(VERSION 3.22.1)
project(ngirim_native CXX)

include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(ngirim SHARED
    native-lib.cpp
    i18n.cpp
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)

target_include_directories(ngirim PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(ngirim PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(ngirim GLESv3 EGL android log)
# Android 15: dukung page size 16 KB
target_link_options(ngirim PRIVATE -Wl,-z,max-page-size=16384)
