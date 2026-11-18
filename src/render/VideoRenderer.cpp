
#include "VideoRenderer.h"
#include <cstring>
#include <thread>
#include <time.h>

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <android/log.h>

#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VideoRenderer", __VA_ARGS__)
#endif
#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VideoRenderer", __VA_ARGS__)
#endif

// EGL/GL 错误检查宏
#define CHECK_EGL_ERROR(msg) \
    do { \
        EGLint err = eglGetError(); \
        if (err != EGL_SUCCESS) { \
            LOGE("%s: EGL error 0x%x", msg, err); \
        } \
    } while(0)

#define CHECK_GL_ERROR(msg) \
    do { \
        GLenum err = glGetError(); \
        if (err != GL_NO_ERROR) { \
            LOGE("%s: GL error 0x%x", msg, err); \
        } \
    } while(0)

// ========== 顶点着色器 ==========
static const char* kVertexShader = R"(
attribute vec4 aPos;
attribute vec2 aTex;
varying vec2 vTex;
void main() {
    gl_Position = aPos;
    vTex = aTex;
}
)";

// ========== 片元着色器（三纹理YUV转RGB + Y轴翻转）==========
static const char* kFragmentShader = R"(
precision mediump float;
varying vec2 vTex;
uniform sampler2D texY;
uniform sampler2D texU;
uniform sampler2D texV;
void main() {
    // 翻转Y坐标：OpenGL ES纹理坐标(0,0)在左下角，视频数据从上往下存储
    vec2 texCoord = vec2(vTex.x, 1.0 - vTex.y);

    // 采样Y、U、V三个纹理（U/V纹理分辨率为Y的一半，但纹理坐标归一化后直接采样即可）
    float y = texture2D(texY, texCoord).r * 255.0;
    float u = texture2D(texU, texCoord).r * 255.0;
    float v = texture2D(texV, texCoord).r * 255.0;

    // YUV -> RGB 转换（与CPU端convertYUV420ToRGB24公式完全一致）
    float r = y + 1.402 * (v - 128.0);
    float g = y + (-0.344136) * (u - 128.0) + (-0.714136) * (v - 128.0);
    float b = y + 1.772 * (u - 128.0);

    // 钳制到[0,255]范围并转回[0,1]
    gl_FragColor = vec4(clamp(r, 0.0, 255.0) / 255.0,
                        clamp(g, 0.0, 255.0) / 255.0,
                        clamp(b, 0.0, 255.0) / 255.0,
                        1.0);
}
)";

static const float kPosData[] = {
    -1.f, -1.f,
     1.f, -1.f,
    -1.f,  1.f,
     1.f,  1.f
};

static const float kTexData[] = {
    0.f, 0.f,
    1.f, 0.f,
    0.f, 1.f,
    1.f, 1.f
};

// 检查 ANativeWindow 是否有效
static bool isNativeWindowValid(ANativeWindow* window) {
    if (!window) return false;
    int32_t w = ANativeWindow_getWidth(window);
    int32_t h = ANativeWindow_getHeight(window);
    int32_t fmt = ANativeWindow_getFormat(window);
    LOGI("VideoRenderer: ANativeWindow check: width=%d, height=%d, format=%d", w, h, fmt);
    return (w > 0 && h > 0 && fmt > 0);
}

// 打印 EGL config 属性用于调试
static void printEGLConfig(EGLDisplay display, EGLConfig config) {
    EGLint red_size = 0, green_size = 0, blue_size = 0, alpha_size = 0;
    EGLint surface_type = 0, renderable_type = 0;
    eglGetConfigAttrib(display, config, EGL_RED_SIZE, &red_size);
    eglGetConfigAttrib(display, config, EGL_GREEN_SIZE, &green_size);
    eglGetConfigAttrib(display, config, EGL_BLUE_SIZE, &blue_size);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha_size);
    eglGetConfigAttrib(display, config, EGL_SURFACE_TYPE, &surface_type);
    eglGetConfigAttrib(display, config, EGL_RENDERABLE_TYPE, &renderable_type);
    LOGI("VideoRenderer: EGL config: R%d G%d B%d A%d surface=0x%x renderable=0x%x",
         red_size, green_size, blue_size, alpha_size, surface_type, renderable_type);
}

// 打印 EGL surface 属性用于调试
static void printEGLSurfaceInfo(EGLDisplay display, EGLSurface surface) {
    EGLint width = 0, height = 0;
    if (eglQuerySurface(display, surface, EGL_WIDTH, &width)) {
        LOGI("VideoRenderer: EGL surface width=%d", width);
    }
    if (eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        LOGI("VideoRenderer: EGL surface height=%d", height);
    }
    // EGLint format = 0;
    // if (eglQuerySurface(display, surface, EGL_FORMAT, &format)) {
    //     LOGI("VideoRenderer: EGL surface format=0x%x", format);
    // }
}

// 高精度计时工具
static long long nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

#endif

namespace player_sdk {

VideoRenderer::~VideoRenderer() {
    release();
}

bool VideoRenderer::init(NativeWindowPtr window) {
#ifdef __ANDROID__
    LOGI("VideoRenderer::init: window=%p", window);
    if (!window) {
        LOGE("VideoRenderer::init: window is null");
        return false;
    }

    ANativeWindow* nw = reinterpret_cast<ANativeWindow*>(window);
    if (!isNativeWindowValid(nw)) {
        LOGE("VideoRenderer::init: ANativeWindow is invalid");
        return false;
    }

    LOGI("VideoRenderer::init: window valid, width=%d, height=%d, format=%d",
         ANativeWindow_getWidth(nw), ANativeWindow_getHeight(nw),
         ANativeWindow_getFormat(nw));

    return initEgl(window);
#else
    (void)window;
    m_inited = true;
    return true;
#endif
}

#ifdef __ANDROID__
bool VideoRenderer::initEgl(NativeWindowPtr window) {
    if (!window) return false;
    m_window = window;

    m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        LOGE("VideoRenderer: eglGetDisplay failed");
        return false;
    }
    LOGI("VideoRenderer: eglGetDisplay OK");

    EGLint major = 0, minor = 0;
    if (!eglInitialize((EGLDisplay)m_eglDisplay, &major, &minor)) {
        LOGE("VideoRenderer: eglInitialize failed");
        return false;
    }
    LOGI("VideoRenderer: EGL initialized: version %d.%d", major, minor);

    // 先枚举所有可用config
    EGLint numConfigsTotal = 0;
    eglGetConfigs((EGLDisplay)m_eglDisplay, nullptr, 0, &numConfigsTotal);
    LOGI("VideoRenderer: Total available configs: %d", numConfigsTotal);

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig((EGLDisplay)m_eglDisplay, attribs, &config, 1, &numConfigs) || numConfigs <= 0) {
        LOGE("VideoRenderer: eglChooseConfig failed");
        return false;
    }
    LOGI("VideoRenderer: eglChooseConfig OK, numConfigs=%d", numConfigs);
    printEGLConfig((EGLDisplay)m_eglDisplay, config);

    EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    m_eglContext = eglCreateContext((EGLDisplay)m_eglDisplay, config, EGL_NO_CONTEXT, ctxAttribs);
    if (m_eglContext == EGL_NO_CONTEXT) {
        LOGE("VideoRenderer: eglCreateContext failed");
        EGLint err = eglGetError();
        LOGE("VideoRenderer: eglCreateContext EGL error: 0x%x", err);
        return false;
    }
    LOGI("VideoRenderer: eglCreateContext OK");

    // 创建窗口surface
    m_eglSurface = eglCreateWindowSurface((EGLDisplay)m_eglDisplay, config, (ANativeWindow*)window, nullptr);
    if (m_eglSurface == EGL_NO_SURFACE) {
        LOGE("VideoRenderer: eglCreateWindowSurface failed");
        EGLint err = eglGetError();
        LOGE("VideoRenderer: eglCreateWindowSurface EGL error: 0x%x", err);
        return false;
    }
    LOGI("VideoRenderer: eglCreateWindowSurface OK");

    // 验证surface属性
    printEGLSurfaceInfo((EGLDisplay)m_eglDisplay, (EGLSurface)m_eglSurface);

    // 检查是否有deferred error
    EGLint errAfterCreate = eglGetError();
    if (errAfterCreate != EGL_SUCCESS) {
        LOGE("VideoRenderer: eglGetError after surface create: 0x%x", errAfterCreate);
    }

    // 使上下文在当前线程生效
    if (!eglMakeCurrent((EGLDisplay)m_eglDisplay, (EGLSurface)m_eglSurface,
                        (EGLSurface)m_eglSurface, (EGLContext)m_eglContext)) {
        LOGE("VideoRenderer: eglMakeCurrent (init) failed");
        EGLint err = eglGetError();
        LOGE("VideoRenderer: eglMakeCurrent EGL error: 0x%x", err);
        return false;
    }
    LOGI("VideoRenderer: eglMakeCurrent (init) OK");

    // 关闭VSync：避免与播放器内部帧率控制产生双重限流导致播放变慢
    EGLBoolean swapIntervalResult = eglSwapInterval((EGLDisplay)m_eglDisplay, 0);
    if (!swapIntervalResult) {
        LOGE("VideoRenderer: eglSwapInterval failed, error=0x%x", eglGetError());
    } else {
        LOGI("VideoRenderer: eglSwapInterval set to 0 (VSync OFF)");
    }

    bool result = initGl();

    // 释放上下文
    eglMakeCurrent((EGLDisplay)m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    LOGI("VideoRenderer: eglMakeCurrent (release after init) OK");

    LOGI("VideoRenderer::initEgl: result=%d", result);
    return result;
}

bool VideoRenderer::initGl() {
    // 编译顶点着色器
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &kVertexShader, nullptr);
    glCompileShader(vs);

    GLint vsCompiled = GL_FALSE;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &vsCompiled);
    if (!vsCompiled) {
        GLchar log[512];
        glGetShaderInfoLog(vs, 512, nullptr, log);
        LOGE("VideoRenderer: Vertex shader compile failed: %s", log);
        glDeleteShader(vs);
        return false;
    }
    LOGI("VideoRenderer: Vertex shader compiled OK");

    // 编译片元着色器
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &kFragmentShader, nullptr);
    glCompileShader(fs);

    GLint fsCompiled = GL_FALSE;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &fsCompiled);
    if (!fsCompiled) {
        GLchar log[512];
        glGetShaderInfoLog(fs, 512, nullptr, log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    LOGI("VideoRenderer: Fragment shader compiled OK");

    // 链接程序
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glBindAttribLocation(m_program, 0, "aPos");
    glBindAttribLocation(m_program, 1, "aTex");
    glLinkProgram(m_program);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[512];
        glGetProgramInfoLog(m_program, 512, nullptr, log);
        LOGE("VideoRenderer: Program link failed: %s", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    LOGI("VideoRenderer: Program linked OK");

    glDeleteShader(vs);
    glDeleteShader(fs);

    // 查询3个uniform位置 (texY, texU, texV)
    GLint texYLoc = glGetUniformLocation(m_program, "texY");
    GLint texULoc = glGetUniformLocation(m_program, "texU");
    GLint texVLoc = glGetUniformLocation(m_program, "texV");
    LOGI("VideoRenderer: uniforms - texY=%d, texU=%d, texV=%d", texYLoc, texULoc, texVLoc);

    // 创建Y纹理 (全分辨率)
    glGenTextures(1, &m_texY);
    glBindTexture(GL_TEXTURE_2D, m_texY);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 创建U纹理 (半分辨率)
    glGenTextures(1, &m_texU);
    glBindTexture(GL_TEXTURE_2D, m_texU);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // 创建V纹理 (半分辨率)
    glGenTextures(1, &m_texV);
    glBindTexture(GL_TEXTURE_2D, m_texV);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glClearColor(0.f, 0.f, 0.f, 1.f);
    CHECK_GL_ERROR("After glClearColor");

    m_inited = true;
    LOGI("VideoRenderer: GL initialized, program=%u, textures Y=%u U=%u V=%u",
         m_program, m_texY, m_texU, m_texV);
    return true;
}

bool VideoRenderer::render(const VideoFrame& frame) {
    if (!m_inited || frame.yuv.empty()) {
        LOGE("VideoRenderer::render: not initialized or empty frame (inited=%d, empty=%d)",
             m_inited, frame.yuv.empty());
        return false;
    }

    LOGI("VideoRenderer::render: width=%d, height=%d, yuv_size=%zu",
         frame.width, frame.height, frame.yuv.size());

    // 高精度计时：用于FPS诊断
    long long t0 = nowUs();

    // 使 EGL 上下文在此线程生效
    if (!eglMakeCurrent((EGLDisplay)m_eglDisplay, (EGLSurface)m_eglSurface,
                        (EGLSurface)m_eglSurface, (EGLContext)m_eglContext)) {
        LOGE("VideoRenderer: eglMakeCurrent (render) failed");
        EGLint err = eglGetError();
        LOGE("VideoRenderer: eglMakeCurrent EGL error: 0x%x", err);
        return false;
    }
    LOGI("VideoRenderer: eglMakeCurrent (render) OK");

    // 分辨率变化时记录
    if (frame.width != m_curWidth || frame.height != m_curHeight) {
        m_curWidth = frame.width;
        m_curHeight = frame.height;
        LOGI("VideoRenderer: video resolution changed to %dx%d", m_curWidth, m_curHeight);
    }

    // 使用 NativeWindow 实际尺寸设置视口
    ANativeWindow* win = reinterpret_cast<ANativeWindow*>(m_window);
    int viewportWidth = frame.width;
    int viewportHeight = frame.height;
    if (win) {
        int winWidth = ANativeWindow_getWidth(win);
        int winHeight = ANativeWindow_getHeight(win);
        // 检查窗口尺寸是否有效
        if (winWidth > 0 && winHeight > 0) {
            viewportWidth = winWidth;
            viewportHeight = winHeight;
            LOGI("VideoRenderer: viewport set to window size %dx%d", viewportWidth, viewportHeight);
        } else {
            LOGE("VideoRenderer: window size invalid (%dx%d), using video size", winWidth, winHeight);
        }
    } else {
        LOGI("VideoRenderer: window is null, using video size %dx%d", viewportWidth, viewportHeight);
    }

    glViewport(0, 0, viewportWidth, viewportHeight);

    // 解析YUV420 Planar数据布局 (I420格式)
    // Y平面: width x height
    // U平面: (width/2) x (height/2)
    // V平面: (width/2) x (height/2)
    const uint8_t* yPlane = frame.yuv.data();
    const uint8_t* uPlane = frame.yuv.data() + frame.width * frame.height;
    const uint8_t* vPlane = frame.yuv.data() + frame.width * frame.height * 5 / 4;

    int halfW = frame.width / 2;
    int halfH = frame.height / 2;

    // 上传Y纹理 (全分辨率) 到 texture unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, yPlane);
    CHECK_GL_ERROR("After Y texImage2D");

    // 上传U纹理 (半分辨率) 到 texture unit 1
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_texU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, halfW, halfH, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, uPlane);
    CHECK_GL_ERROR("After U texImage2D");

    // 上传V纹理 (半分辨率) 到 texture unit 2
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_texV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, halfW, halfH, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, vPlane);
    CHECK_GL_ERROR("After V texImage2D");

    LOGI("VideoRenderer: YUV textures uploaded (Y=%dx%d, U/V=%dx%d)",
         frame.width, frame.height, halfW, halfH);

    // 使用着色器程序渲染
    glUseProgram(m_program);
    glUniform1i(glGetUniformLocation(m_program, "texY"), 0);
    glUniform1i(glGetUniformLocation(m_program, "texU"), 1);
    glUniform1i(glGetUniformLocation(m_program, "texV"), 2);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kPosData);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, kTexData);
    glEnableVertexAttribArray(1);

    // 清除并绘制
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    CHECK_GL_ERROR("After drawArrays");

    // 交换缓冲区（VSync已关闭，帧率由播放器内部计时控制）
    EGLBoolean swapResult = eglSwapBuffers((EGLDisplay)m_eglDisplay, (EGLSurface)m_eglSurface);
    if (swapResult != EGL_TRUE) {
        EGLint err = eglGetError();
        LOGE("VideoRenderer: eglSwapBuffers failed: result=%d, error=0x%x", swapResult, err);
    } else {
        LOGI("VideoRenderer::render: frame rendered OK");
    }

    // FPS诊断日志：每30帧打印一次平均帧时间和近似FPS
    long long t1 = nowUs();
    long long renderUs = t1 - t0;
    static int frameCount = 0;
    static long long totalUs = 0;
    frameCount++;
    totalUs += renderUs;
    if (frameCount % 30 == 0) {
        long long avgUs = totalUs / frameCount;
        double fps = 1000000.0 / (double)avgUs;
        LOGI("[TIMING] render avg=%lld us, last=%lld us, approx fps=%.2f",
             avgUs, renderUs, fps);
    }

    // 渲染完成后释放上下文
    eglMakeCurrent((EGLDisplay)m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    return true;
}

void VideoRenderer::releaseEgl() {
    eglMakeCurrent((EGLDisplay)m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (m_program) {
        glDeleteProgram(m_program);
        m_program = 0;
    }
    if (m_texY) {
        glDeleteTextures(1, &m_texY);
        m_texY = 0;
    }
    m_texU = 0;
    m_texV = 0;

    if (m_eglDisplay) {
        if (m_eglSurface) eglDestroySurface((EGLDisplay)m_eglDisplay, (EGLSurface)m_eglSurface);
        if (m_eglContext) eglDestroyContext((EGLDisplay)m_eglDisplay, (EGLContext)m_eglContext);
        eglTerminate((EGLDisplay)m_eglDisplay);
    }
    m_eglDisplay = m_eglContext = m_eglSurface = nullptr;
    LOGI("VideoRenderer: EGL resources released");
}
#else
bool VideoRenderer::render(const VideoFrame& frame) {
    (void)frame;
    return m_inited;
}
#endif

void VideoRenderer::release() {
#ifdef __ANDROID__
    releaseEgl();
    m_window = nullptr;
#endif
    m_inited = false;
}

} // namespace player_sdk

//看了你的代码，这段是一个 Android 上用 OpenGL ES 渲染 YUV 视频帧 的 C++ 实现。你觉得绕很正常，因为它把 EGL（OpenGL 和系统窗口之间的"桥梁"）和 OpenGL 渲染管线全揉在一起了。

// 第一条线： EGL 初始化（搭建舞台）
//
// initEgl() 做的事情，类比成"搭一个舞台准备演戏"：
// 步骤   代码   人话
// ① 拿到显示器   eglGetDisplay   找到屏幕（就像找到一块画布）
//
// ② 启动 EGL   eglInitialize   初始化 OpenGL ES 环境
//
// ③ 选配置   eglChooseConfig   告诉系统"我要 OpenGL ES 2.0 + 窗口类型"
//
// ④ 创建上下文   eglCreateContext   创建"画笔"（GL 上下文，后续所有绘制操作都靠它）
//
// ⑤ 创建表面   eglCreateWindowSurface   把 ANativeWindow（Surface）绑定到 EGL，告诉 OpenGL"画到哪个窗口上"
//
// ⑥ 绑定上下文到线程   eglMakeCurrent   把画笔交给当前线程，之后这个线程就能画了
//
// ⑦ 关 VSync   eglSwapInterval(0)   关掉屏幕刷新同步，让播放器自己控制帧率（避免双重限流导致卡顿）
//
// 关键理解：EGL 就是 OpenGL 和 Android 窗口系统之间的"翻译官"。OpenGL 本身不知道 Android 的 Surface 是什么，EGL 负责把它们连起来。
//
// 第二条线： GL 初始化（准备颜料和画板）
//
// initGl() 做的事情：
//
// 编译着色器（Shader）：把顶点着色器和片元着色器的 GLSL 代码交给 GPU 编译
// 链接程序：把两个着色器连成一个可执行的 GPU 程序
// 创建 3 个纹理对象：m_texY、m_texU、m_texV，对应 YUV 的 Y、U、V 三个分量
// 设置纹理参数：线性滤波 + 边缘夹紧（防止纹理拉伸时出现杂色）
//
// 关键理解：这时候 GPU 已经准备好了"怎么画"（着色器）和"画在哪"（纹理对象），但还没有实际数据。
//
// 第三条线： 渲染一帧（真正作画）
//
// render() 是核心，每来一帧视频数据就走一遍。按顺序拆：
//
// Step 1：激活上下文
// eglMakeCurrent(...)  // 让当前线程拿到画笔
//
// Step 2：解析 YUV 数据内存布局
// YUV420 Planar(I420) 内存排列：
// ┌──────────────────────────────┐
// │ YYYYYYYY...  (width×height)   │  ← yPlane
// │ UUUU...        (w/2×h/2)      │  ← uPlane
// │ VVVV...        (w/2×h/2)      │  ← vPlane
// └──────────────────────────────┘
//
// 代码里用指针偏移找到三个平面的起始地址：
// yPlane = frame.yuv.data() —— Y 平面从头开始
// uPlane = yPlane + width×height —— U 平面在 Y 后面
// vPlane = yPlane + width×height×5/4 —— V 平面在 U 后面（因为 U 占了 width×height×1/4 的空间）
//
// Step 3：把数据上传到 GPU 纹理
// glActiveTexture(GL_TEXTURE0);  // 选第0号纹理单元
// glBindTexture(GL_TEXTURE_2D, m_texY);  // 绑定Y纹理
// glTexImage2D(...);  // 把Y平面数据拷贝到GPU显存
//
// // U、V 同理，但分辨率是半宽半高
//
// 关键理解：glTexImage2D 是把 CPU 内存的数据拷贝到 GPU 显存的过程。Y 是全分辨率，U/V 是半分辨率（因为 420 格式里 U/V 采样率是 Y 的一半）。
//
// Step 4：告诉 GPU 怎么画
// glUseProgram(m_program);           // 启用我们编译好的着色器程序
// glUniform1i(...texY..., 0);        // 告诉着色器：texY 用纹理单元0
// glUniform1i(...texU..., 1);        // texU 用纹理单元1
// glUniform1i(...texV..., 2);        // texV 用纹理单元2
//
// glVertexAttribPointer(0, ...);     // 设置顶点位置属性（4个角）
// glVertexAttribPointer(1, ...);     // 设置纹理坐标属性
// glEnableVertexAttribArray(0, 1);   // 启用这两个属性
//
// glClear(GL_COLOR_BUFFER_BIT);      // 清屏
// glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);  // 画一个四边形（覆盖整个屏幕）
//
// 关键理解：这里画的是一个全屏四边形（4个顶点：左上、右上、左下、右下），GPU 会自动填充四边形内部的像素——填充时就会执行片元着色器里的 YUV→RGB 转换公式。
//
// Step 5：交换缓冲区（显示出来）
// eglSwapBuffers(display, surface);  // 把 GPU 画好的内容显示到屏幕上
//
// 关键理解：OpenGL 用的是双缓冲——GPU 在前缓冲区（Back Buffer）画，画完了调用 swapBuffers 把前后缓冲区交换，用户就看到新画面了。
//
// 第四条线： 片元着色器（YUV→RGB 的核心魔法）
//
// 这才是视频渲染最核心的部分。着色器里做的事情：
//
// // 1. 翻转Y坐标（OpenGL纹理坐标原点在左下角，视频数据从上往下存）
// vec2 texCoord = vec2(vTex.x, 1.0 - vTex.y);
//
// // 2. 从3个纹理采样Y、U、V值
// float y = texture2D(texY, texCoord).r * 255.0;
// float u = texture2D(texU, texCoord).r * 255.0;
// float v = texture2D(texV, texCoord).r * 255.0;
//
// // 3. YUV → RGB 转换公式（ITU-R BT.601 标准）
// float r = y + 1.402 * (v - 128.0);
// float g = y + (-0.344136) * (u - 128.0) + (-0.714136) * (v - 128.0);
// float b = y + 1.772 * (u - 128.0);
//
// // 4. 钳制到合法范围并输出
// gl_FragColor = vec4(clamp(r,0,255)/255.0, clamp(g,0,255)/255.0, clamp(b,0,255)/255.0, 1.0);
//
// 关键理解：
// 视频数据存的是 YUV 格式（亮度+色度分离，节省带宽）
// 屏幕需要的是 RGB 格式（红绿蓝三色）
// 着色器就是做格式转换的，而且是在 GPU 上并行做（每个像素同时算）
// 1.402、0.344136、1.772 这些数字是 ITU-R BT.601 标准里的转换系数，面试可以提一句"用的是 BT.601 标准"
//
// 整体流程图（面试可以画这个）
//
// 解码器输出 YUV帧
//     ↓
// CPU 内存 (YUV420 Planar)
//     ↓  glTexImage2D (拷贝到GPU显存)
// GPU 纹理 (Y纹理 + U纹理 + V纹理)
//     ↓  glDrawArrays 触发着色器执行
// GPU 片元着色器 (YUV→RGB 转换)
//     ↓
// GPU 帧缓冲区 (Back Buffer)
//     ↓  eglSwapBuffers (前后缓冲交换)
// 屏幕显示
//
// 面试可能追问的点
//
// 为什么用 GPU 做 YUV→RGB 而不是 CPU？
//    — GPU 并行计算能力远强于 CPU，一帧 1920×1080 有 200 多万个像素要转换，CPU 做会卡顿。
//
// 为什么 U/V 是半分辨率？
//    — 人眼对亮度（Y）更敏感，对色度（U/V）不那么敏感，所以 4:2:0 采样把色度分辨率降一半来节省带宽和存储。
//
// glTexImage2D 每帧都调会不会很慢？
//    — 确实有性能开销。优化方案是用 glTexSubImage2D（只更新变化部分）或者用 EGLImage + OpenGL 共享内存避免拷贝（零拷贝渲染）。
//
// 关掉 VSync 有什么风险？
//    — 可能出现画面撕裂（frame tearing），但播放器场景通常可以接受，因为播放器自己做了帧率控制。
//
