cmake_minimum_required(VERSION 3.22)
project(RayPlayer)

set(CMAKE_CXX_STANDARD 20)

# ==========================================
# 1. 头文件与 FFmpeg 路径配置
# ==========================================
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/include)

set(FFMPEG_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/libs/${ANDROID_ABI})

# ==========================================
# 2. 定义你的共享库
# ==========================================
# 注意：这里把 Player.cpp 放到了 include 里，CMake 默认也会去 include 目录找源文件，
# 但建议以后把 Player.cpp 移到 src 目录下，保持项目规范。
add_library(RayPlayer SHARED
        src/core/PlayerImpl.cpp
        src/core/Clock.cpp
        include/player/Player.cpp
        src/render/AudioRenderer.cpp
        src/decoder/AudioDecoder.cpp
        src/decoder/VideoDecoder.cpp
        src/render/VideoRenderer.cpp
        src/jni/rayplayer_jni.cpp
)

# ==========================================
# 3. 导入合并后的 FFmpeg 动态库
# ==========================================
add_library(ffmpeg SHARED IMPORTED)
set_target_properties(ffmpeg PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libffmpeg.so
)

# ==========================================
# 4. 链接所有依赖
# ==========================================
if(ANDROID)
    # 找到 Android 系统库
    find_library(log-lib log)
    find_library(android-lib android)

    target_link_libraries(RayPlayer PRIVATE
            ffmpeg      # 【核心修改】直接链接合并好的 ffmpeg 库
            ${log-lib}
            ${android-lib}
            aaudio      # 【注意】Android 系统库直接用名称即可，CMake 自动找
            EGL
            GLESv2
    )
else()
    # Mac 本地编译逻辑
    message(STATUS "非Android环境，跳过 log 库")

    target_link_libraries(RayPlayer
            ffmpeg
    )
endif()