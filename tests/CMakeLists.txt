cmake_minimum_required(VERSION 3.20)
project(krkr_ns_port_tests LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 17)
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(SDL2 CONFIG REQUIRED)
set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../krkrsdl2")
add_executable(bitmap_bridge_test bitmap_bridge_test.cpp)
target_include_directories(bitmap_bridge_test PRIVATE "${PORT_ROOT}/src/core/sdl2")
target_compile_definitions(bitmap_bridge_test PRIVATE SDL_MAIN_HANDLED NOMINMAX)
target_link_libraries(bitmap_bridge_test PRIVATE SDL2::SDL2)
add_test(NAME bitmap_bridge COMMAND bitmap_bridge_test)
add_executable(emote_gl_test emote_gl_test.cpp
    "${PORT_ROOT}/src/plugins/emoteplayer/EmoteGLRenderBackend.cpp"
    "${PORT_ROOT}/src/plugins/emoteplayer/EmoteSWRenderBackend.cpp")
target_include_directories(emote_gl_test PRIVATE
    "${PORT_ROOT}/src/plugins/emoteplayer" "${PORT_ROOT}/src/core/sdl2")
target_compile_definitions(emote_gl_test PRIVATE SDL_MAIN_HANDLED NOMINMAX)
target_link_libraries(emote_gl_test PRIVATE SDL2::SDL2)
add_test(NAME emote_gl COMMAND emote_gl_test)
add_test(NAME emote_cpu COMMAND emote_gl_test --cpu)
if(WIN32)
    add_custom_command(TARGET bitmap_bridge_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E
        copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:bitmap_bridge_test>)
    add_custom_command(TARGET emote_gl_test POST_BUILD COMMAND ${CMAKE_COMMAND} -E
        copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:emote_gl_test>)
endif()
