cmake_minimum_required(VERSION 3.20)
project(AviationComms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.3.8
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

find_package(OpenGL REQUIRED)

set(IMGUI_SOURCES
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/imgui_impl_glfw.cpp
  third_party/imgui/imgui_impl_opengl3.cpp
)

add_library(common STATIC
  common/logger.cpp
)
target_include_directories(common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

add_executable(aircraft_client
  client/main.cpp
)
target_link_libraries(aircraft_client PRIVATE common)

add_executable(ground_server
  server/main.cpp
  server/state_machine.cpp
  server/imgui_dashboard.cpp
  ${IMGUI_SOURCES}
)
target_include_directories(ground_server
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
)
target_link_libraries(ground_server PRIVATE common glfw OpenGL::GL)

if(WIN32)
  target_compile_definitions(common INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(aircraft_client PRIVATE ws2_32)
  target_link_libraries(ground_server PRIVATE ws2_32)
endif()

if(APPLE)
  target_compile_definitions(ground_server PRIVATE GL_SILENCE_DEPRECATION)
endif()
