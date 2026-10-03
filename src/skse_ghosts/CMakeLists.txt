# DDDAGhosts: SKSE plugin (CommonLibSSE, powerof3 fork, Skyrim 1.7.104) that puts
# invisible "ghost" actors in Skyrim for DDDA's pawns. Build with build_skse.bat.
cmake_minimum_required(VERSION 3.21)
project(DDDAGhosts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(SKYRIM_SUPPORT_AE ON CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../external/commonlibsse-po3" commonlibsse EXCLUDE_FROM_ALL)

add_library(DDDAGhosts SHARED plugin.cpp havok_export.cpp)
target_include_directories(DDDAGhosts PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_link_libraries(DDDAGhosts PRIVATE CommonLibSSE::CommonLibSSE)
target_compile_options(DDDAGhosts PRIVATE /W4 /EHsc /permissive- /utf-8 /external:anglebrackets /external:W0)
