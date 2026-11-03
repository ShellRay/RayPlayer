
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