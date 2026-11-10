#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct OH_AVCodec;
struct OH_AVBuffer;
struct NativeWindow;

// Speaks the scrcpy v5.0.1 protocol (video + control sockets) through the
// gateway, decodes H.264 with the hardware AVCodec decoder straight into the
// XComponent surface, and serializes touch/key control messages.
class ScrcpyClient {
public:
    using StatusCallback = std::function<void(const std::string &status, int width, int height)>;

    static ScrcpyClient &Instance();

    void SetWindow(NativeWindow *window);
    bool Connect(const std::string &host, int port, StatusCallback cb);
    void Disconnect();

    // action: 0 down, 1 up, 2 move, 3 cancel. x/y in surface pixels.
    void SendTouch(int action, int64_t pointerId, float x, float y, float surfaceW, float surfaceH);
    void SendKey(int keycode);
    void SendText(const std::string &utf8);
    // Ask the gateway to resize the Android display (pixels, dpi). Sent right
    // away when streaming, otherwise kept until the handshake is done.
    void RequestResize(int width, int height, int dpi);

private:
    ScrcpyClient() = default;
    void VideoLoop();
    bool StartDecoder(int width, int height);
    void StopDecoder();
    void FeedPacket(const uint8_t *data, size_t size, int64_t pts, uint32_t flags);
    void SendControl(const std::vector<uint8_t> &msg);
    void Report(const std::string &status);
    void FlushResize();

    static void OnError(OH_AVCodec *codec, int32_t err, void *ud);
    static void OnStreamChanged(OH_AVCodec *codec, struct OH_AVFormat *fmt, void *ud);
    static void OnNeedInput(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buf, void *ud);
    static void OnNewOutput(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buf, void *ud);

    std::mutex mutex_;
    NativeWindow *window_ = nullptr;
    int videoFd_ = -1;
    int controlFd_ = -1;
    std::mutex controlMutex_;
    std::thread videoThread_;
    std::atomic<bool> running_{false};
    StatusCallback callback_;

    OH_AVCodec *decoder_ = nullptr;
    std::mutex inputMutex_;
    std::condition_variable inputCv_;
    std::deque<std::pair<uint32_t, OH_AVBuffer *>> freeInputs_;

    bool streaming_ = false; // guarded by controlMutex_
    int resizeW_ = 0;        // guarded by controlMutex_
    int resizeH_ = 0;
    int resizeDpi_ = 0;

    std::atomic<int> videoW_{0};
    std::atomic<int> videoH_{0};
};
