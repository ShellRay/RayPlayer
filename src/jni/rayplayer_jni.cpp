
//
// Created by 谢稳 on 2026/10/23.
//
#include <jni.h>
#include <string>
#include "../src/core/PlayerImpl.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>

// 保存当前播放器的 Native 实例指针
static player_sdk::PlayerImpl* g_player = nullptr;

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeSetSurface(JNIEnv *env, jobject thiz, jobject surface) {
    ANativeWindow* window = nullptr;
    if (surface != nullptr) {
        window = ANativeWindow_fromSurface(env, surface);
    }
    // 将窗口指针传给 PlayerImpl，由 PlayerImpl 管理引用计数
    if (g_player != nullptr) {
        g_player->setVideoSurface(window);
    } else {
        // 如果播放器未初始化，释放窗口引用避免泄漏
        if (window != nullptr) {
            ANativeWindow_release(window);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeUpdateSurface(JNIEnv *env, jobject thiz, jobject surface) {
    ANativeWindow* window = nullptr;
    if (surface != nullptr) {
        window = ANativeWindow_fromSurface(env, surface);
    }
    // 将窗口指针传给 PlayerImpl，由 PlayerImpl 管理引用计数
    if (g_player != nullptr) {
        g_player->updateSurface(window);
    } else {
        // 如果播放器未初始化，释放窗口引用避免泄漏
        if (window != nullptr) {
            ANativeWindow_release(window);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeInit(JNIEnv *env, jobject thiz) {
    if (g_player == nullptr) {
        g_player = new player_sdk::PlayerImpl();
    }
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativePrepare(JNIEnv *env, jobject thiz, jstring j_path) {
    if (g_player == nullptr) return;

    const char *path = env->GetStringUTFChars(j_path, nullptr);
    g_player->setDataSource(path);
    g_player->prepare();
    env->ReleaseStringUTFChars(j_path, path);
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeStart(JNIEnv *env, jobject thiz) {
    if (g_player != nullptr) {
        g_player->start();
    }
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeStop(JNIEnv *env, jobject thiz) {
    if (g_player != nullptr) {
        g_player->stop();
    }
}

JNIEXPORT void JNICALL
Java_com_example_rayplayer_RayPlayer_nativeRelease(JNIEnv *env, jobject thiz) {
    if (g_player != nullptr) {
        g_player->release();
        delete g_player;
        g_player = nullptr;
    }
}

} // extern "C"