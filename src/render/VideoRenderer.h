
#ifndef RAYPLAYER_VIDEO_RENDERER_H
#define RAYPLAYER_VIDEO_RENDERER_H

#include <string>
#include <vector>
#include "../src/decoder/VideoDecoder.h"

typedef void* NativeWindowPtr;

namespace player_sdk {

    class VideoRenderer {
    public:
        ~VideoRenderer();

        bool init(NativeWindowPtr window);
        bool render(const VideoFrame& frame);
        void release();

    private:
        bool initEgl(NativeWindowPtr window);
        bool initGl();
        void releaseEgl();

        void* m_eglDisplay = nullptr;
        void* m_eglContext = nullptr;
        void* m_eglSurface = nullptr;
        void* m_window = nullptr;

        unsigned int m_program = 0;
        unsigned int m_texY = 0;
        unsigned int m_texU = 0;
        unsigned int m_texV = 0;
        int m_curWidth = 0;
        int m_curHeight = 0;
        bool m_inited = false;
    };

} // namespace player_sdk

#endif // RAYPLAYER_VIDEO_RENDERER_H