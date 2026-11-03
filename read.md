player-sdk/
├── CMakeLists.txt
├── include/player/
│   ├── Player.h            对外C++接口
│   ├── PlayerListener.h    回调
│   └── PlayerConfig.h      配置
├── src/core/
│   ├── PlayerImpl.cpp      状态机与调度
│   └── Clock.cpp           主时钟
├── src/io/                 IDataSource、FileSource、HttpSource
├── src/demuxer/            IDemuxer、FFmpegDemuxer、WavDemuxer
├── src/decoder/            IDecoder、FFmpegAudioDecoder、MediaCodecDecoder
├── src/render/             IAudioRenderer、OboeRenderer、AAudioRenderer
├── src/buffer/             PacketQueue、FrameQueue、RingBuffer
├── src/sync/               AVSync
├── src/platform/           JNI桥接、Android/iOS/HarmonyOS适配
└── demo/


[容器文件/网络流]
│
▼
┌─────────┐
│ doPrepare │  ← 已写完：avformat_open_input + avformat_find_stream_info
└────┬────┘
│ m_fmtCtx 准备好了
▼
┌─────────┐
│ doDemux  │  ← 本次新增：av_read_frame 循环读取，分发到音视频队列
└────┬────┘
│
├─→ m_audioPacketQueue ──→ popAudioPacket() ──→ 音频解码器（下一步写）
│
└─→ m_videoPacketQueue ──→ popVideoPacket() ──→ 视频解码器（下一步写）

[容器文件]
│
▼
┌──────────────┐
│ doPrepare()   │  avformat_open_input + find_stream_info
└──────┬───────┘
│ m_fmtCtx 就绪
▼
┌──────────────┐     ┌─────────────────┐
│ doDemux()    │────▶│ m_audioPacketQueue │
│ (解封装线程)  │     └────────┬────────┘
└──────────────┘              │
▼
┌─────────────────┐
│ doAudioDecode()  │  (音频解码线程)
│ popAudioPacket() │
└────────┬────────┘
│
▼
┌─────────────────┐
│ AudioDecoder     │  avcodec_send_packet
│   ::decode()     │ → avcodec_receive_frame
└────────┬────────┘
│ swr_convert (重采样)
▼
┌─────────────────┐
│ callback         │  
└────────┬────────┘
│
▼
┌─────────────────┐
│ AudioRenderer    │  AAudioStream_write
│   ::write()      │ → 扬声器
└─────────────────┘


[容器文件/网络流]
│
▼
┌──────────────┐
│ doPrepare()   │  avformat_open_input + find_stream_info
│              │  + 初始化 AudioDecoder + VideoDecoder
└──────┬───────┘
│ m_fmtCtx 就绪
▼
┌──────────────┐
│ doDemux()     │  av_read_frame → 分发到音视频队列
│ (解封装线程)   │
└──────┬───────┘
│
├─→ m_audioPacketQueue ──▶ popAudioPacket()
│                            │
│                            ▼
│                    ┌───────────────┐
│                    │ doAudioDecode() │ (音频解码线程)
│                    │ AudioDecoder::decode()
│                    └───────┬────────┘
│                            │ PCM 回调
│                            ▼
│                    ┌───────────────┐
│                    │ AudioRenderer  │ AAudioStream_write
│                    │   ::write()    │ → 扬声器出声
│                    └───────────────┘
│
├─→ m_videoPacketQueue ──▶ popVideoPacket()
│                            │
│                            ▼
│                    ┌───────────────┐
│                    │ doVideoDecode() │ (视频解码线程)
│                    │ VideoDecoder::decode()
│                    └───────┬────────┘
│                            │ YUV 帧回调
│                            ▼
│                    ┌───────────────┐
│                    │ 时钟同步       │ 对比 audioClock vs videoPTS
│                    │ (drop/delay)   │ 超前等/落后跳
│                    └───────┬────────┘
│                            │
│                            ▼
│                    ┌───────────────┐
│                    │ VideoRenderer  │ EGL + OpenGL ES
│                    │   ::render()   │ YUV→RGB shader
│                    └───────┬────────┘
│                            │
▼                            ▼
(扬声器出声)              (Surface → 屏幕显示画面)