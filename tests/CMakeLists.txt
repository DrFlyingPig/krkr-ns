cmake_minimum_required(VERSION 3.20)
project(krkr_ns_port_tests LANGUAGES C CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 17)
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(SDL2 CONFIG REQUIRED)
set(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../krkrsdl2")
set(AMV_ZLIB_SOURCES)
foreach(name adler32 compress crc32 deflate inflate infback inftrees inffast trees uncompr zutil)
    list(APPEND AMV_ZLIB_SOURCES "${PORT_ROOT}/external/zlib/${name}.c")
endforeach()
add_library(amv_test_zlib STATIC ${AMV_ZLIB_SOURCES})
target_include_directories(amv_test_zlib PUBLIC "${PORT_ROOT}/external/zlib")
add_executable(alphamovie_test alphamovie_test.cpp
    "${PORT_ROOT}/src/plugins/alphamovie/AmvMovie.cpp"
    "${PORT_ROOT}/src/plugins/alphamovie/AmvCodec.cpp")
target_include_directories(alphamovie_test PRIVATE "${PORT_ROOT}/src/plugins/alphamovie"
    "${PORT_ROOT}/external/zlib")
target_link_libraries(alphamovie_test PRIVATE amv_test_zlib)
add_test(NAME alphamovie COMMAND alphamovie_test)
add_executable(bitmap_pool_test bitmap_pool_test.cpp)
target_include_directories(bitmap_pool_test PRIVATE "${PORT_ROOT}/src/core/sdl2")
add_test(NAME bitmap_pool COMMAND bitmap_pool_test)
add_executable(menu_perf_test menu_perf_test.cpp)
target_include_directories(menu_perf_test PRIVATE "${PORT_ROOT}/src/core/sdl2" "${PORT_ROOT}/external/krkrz/visual")
add_test(NAME menu_perf COMMAND menu_perf_test)
add_executable(bitmap_bridge_test bitmap_bridge_test.cpp)
target_include_directories(bitmap_bridge_test PRIVATE "${PORT_ROOT}/src/core/sdl2")
target_compile_definitions(bitmap_bridge_test PRIVATE SDL_MAIN_HANDLED NOMINMAX)
target_link_libraries(bitmap_bridge_test PRIVATE SDL2::SDL2)
add_test(NAME bitmap_bridge COMMAND bitmap_bridge_test)
add_executable(emote_gl_test emote_gl_test.cpp
    "${PORT_ROOT}/src/plugins/emoteplayer/EmoteGLRenderBackend.cpp"
    "${PORT_ROOT}/src/plugins/emoteplayer/EmoteSWRenderBackend.cpp")
target_include_directories(emote_gl_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
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
