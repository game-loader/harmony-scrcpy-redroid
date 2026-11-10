#include "scrcpy_client.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include <hilog/log.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

#undef LOG_TAG
#define LOG_TAG "scrcpy"

namespace {
constexpr size_t kDeviceNameLength = 64;
constexpr uint64_t kFlagConfig = 1ULL << 62;
constexpr uint64_t kFlagKeyFrame = 1ULL << 61;
constexpr uint64_t kPtsMask = kFlagKeyFrame - 1;
constexpr size_t kMaxPacketSize = 8 * 1024 * 1024;
constexpr size_t kMaxTextLength = 300;

enum ControlType : uint8_t { kInjectKeycode = 0, kInjectText = 1, kInjectTouch = 2 };

int ConnectTcp(const std::string &host, int port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

bool RecvExact(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t r = recv(fd, buf, len, 0);
        if (r <= 0) return false;
        buf += r;
        len -= static_cast<size_t>(r);
    }
    return true;
}

uint32_t Read32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint64_t Read64(const uint8_t *p) { return (uint64_t(Read32(p)) << 32) | Read32(p + 4); }

void Put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(x >> 8);
    v.push_back(x & 0xFF);
}

void Put32(std::vector<uint8_t> &v, uint32_t x) {
    Put16(v, x >> 16);
    Put16(v, x & 0xFFFF);
}

void Put64(std::vector<uint8_t> &v, uint64_t x) {
    Put32(v, x >> 32);
    Put32(v, x & 0xFFFFFFFF);
}
} // namespace

ScrcpyClient &ScrcpyClient::Instance() {
    static ScrcpyClient client;
    return client;
}

void ScrcpyClient::SetWindow(NativeWindow *window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
}

bool ScrcpyClient::Connect(const std::string &host, int port, StatusCallback cb) {
    Disconnect();
    callback_ = std::move(cb);
    running_ = true;
    videoThread_ = std::thread([this, host, port] {
        Report("connecting");
        int vfd = ConnectTcp(host, port);
        int cfd = vfd >= 0 ? ConnectTcp(host, port) : -1;
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            videoFd_ = vfd;
            controlFd_ = cfd;
        }
        if (vfd < 0 || cfd < 0) {
            Report("error: cannot connect to gateway");
        } else {
            VideoLoop();
        }
        StopDecoder();
        {
            std::lock_guard<std::mutex> lock(controlMutex_);
            if (videoFd_ >= 0) close(videoFd_);
            if (controlFd_ >= 0) close(controlFd_);
            videoFd_ = controlFd_ = -1;
            streaming_ = false;
        }
        bool wasRunning = running_.exchange(false);
        if (wasRunning) Report("disconnected");
    });
    return true;
}

void ScrcpyClient::Disconnect() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        if (videoFd_ >= 0) shutdown(videoFd_, SHUT_RDWR);
        if (controlFd_ >= 0) shutdown(controlFd_, SHUT_RDWR);
    }
    inputCv_.notify_all();
    if (videoThread_.joinable()) videoThread_.join();
}

void ScrcpyClient::Report(const std::string &status) {
    OH_LOG_INFO(LOG_APP, "status: %{public}s", status.c_str());
    if (status.rfind("error", 0) == 0) running_ = false; // keep the error visible instead of "disconnected"
    if (callback_) callback_(status, videoW_, videoH_);
}
void ScrcpyClient::VideoLoop() {
    uint8_t name[kDeviceNameLength];
    uint8_t codecId[4];
    if (!RecvExact(videoFd_, name, sizeof(name)) || !RecvExact(videoFd_, codecId, sizeof(codecId))) {
        Report("error: handshake failed");
        return;
    }
    name[kDeviceNameLength - 1] = 0;
    if (Read32(codecId) != 0x68323634) {
        Report("error: unsupported codec");
        return;
    }
    Report(std::string("connected: ") + reinterpret_cast<char *>(name));
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        streaming_ = true;
    }
    FlushResize();

    std::vector<uint8_t> packet;
    std::vector<uint8_t> config; // SPS/PPS, prepended to the next key frame
    uint8_t header[12];
    while (running_) {
        if (!RecvExact(videoFd_, header, sizeof(header))) break;
        if (header[0] & 0x80) {
            // Session packet: capture (re)started with a new size, e.g. rotation.
            int w = static_cast<int>(Read32(header + 4));
            int h = static_cast<int>(Read32(header + 8));
            StopDecoder();
            videoW_ = w;
            videoH_ = h;
            config.clear();
            if (!StartDecoder(w, h)) {
                Report("error: decoder init failed");
                return;
            }
            Report("size");
            continue;
        }
        uint64_t ptsFlags = Read64(header);
        uint32_t size = Read32(header + 8);
        if (size == 0 || size > kMaxPacketSize) {
            Report("error: bad packet size");
            return;
        }
        packet.resize(size);
        if (!RecvExact(videoFd_, packet.data(), size)) break;
        if (ptsFlags & kFlagConfig) {
            config = packet;
            continue;
        }
        if (!config.empty()) {
            packet.insert(packet.begin(), config.begin(), config.end());
            config.clear();
        }
        uint32_t flags = (ptsFlags & kFlagKeyFrame) ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
        FeedPacket(packet.data(), packet.size(), static_cast<int64_t>(ptsFlags & kPtsMask), flags);
    }
}
bool ScrcpyClient::StartDecoder(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!window_) return false;
    decoder_ = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (!decoder_) return false;
    OH_AVCodecCallback cb = {OnError, OnStreamChanged, OnNeedInput, OnNewOutput};
    OH_VideoDecoder_RegisterCallback(decoder_, cb, this);
    OH_AVFormat *fmt = OH_AVFormat_Create();
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
    bool ok = OH_VideoDecoder_Configure(decoder_, fmt) == AV_ERR_OK &&
              OH_VideoDecoder_SetSurface(decoder_, window_) == AV_ERR_OK &&
              OH_VideoDecoder_Prepare(decoder_) == AV_ERR_OK &&
              OH_VideoDecoder_Start(decoder_) == AV_ERR_OK;
    OH_AVFormat_Destroy(fmt);
    if (!ok) {
        OH_VideoDecoder_Destroy(decoder_);
        decoder_ = nullptr;
    }
    return ok;
}

void ScrcpyClient::StopDecoder() {
    OH_AVCodec *dec;
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        dec = decoder_;
        decoder_ = nullptr;
        freeInputs_.clear();
    }
    inputCv_.notify_all();
    if (dec) {
        OH_VideoDecoder_Stop(dec);
        OH_VideoDecoder_Destroy(dec);
    }
}

void ScrcpyClient::FeedPacket(const uint8_t *data, size_t size, int64_t pts, uint32_t flags) {
    std::unique_lock<std::mutex> lock(inputMutex_);
    inputCv_.wait(lock, [this] { return !running_ || !decoder_ || !freeInputs_.empty(); });
    if (!running_ || !decoder_) return;
    auto [index, buf] = freeInputs_.front();
    freeInputs_.pop_front();
    int32_t cap = OH_AVBuffer_GetCapacity(buf);
    if (static_cast<size_t>(cap) < size) {
        OH_LOG_WARN(LOG_APP, "input buffer too small: %{public}d < %{public}zu", cap, size);
        size = static_cast<size_t>(cap);
    }
    memcpy(OH_AVBuffer_GetAddr(buf), data, size);
    OH_AVCodecBufferAttr attr{pts, static_cast<int32_t>(size), 0, flags};
    OH_AVBuffer_SetBufferAttr(buf, &attr);
    OH_VideoDecoder_PushInputBuffer(decoder_, index);
}

void ScrcpyClient::OnError(OH_AVCodec *, int32_t err, void *) {
    OH_LOG_ERROR(LOG_APP, "decoder error %{public}d", err);
}

void ScrcpyClient::OnStreamChanged(OH_AVCodec *, OH_AVFormat *, void *) {}

void ScrcpyClient::OnNeedInput(OH_AVCodec *, uint32_t index, OH_AVBuffer *buf, void *ud) {
    auto *self = static_cast<ScrcpyClient *>(ud);
    {
        std::lock_guard<std::mutex> lock(self->inputMutex_);
        self->freeInputs_.emplace_back(index, buf);
    }
    self->inputCv_.notify_one();
}

void ScrcpyClient::OnNewOutput(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *, void *) {
    // Render immediately: no client-side buffering, lowest latency.
    OH_VideoDecoder_RenderOutputBuffer(codec, index);
}
void ScrcpyClient::SendControl(const std::vector<uint8_t> &msg) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (controlFd_ < 0) return;
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t r = send(controlFd_, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (r <= 0) return;
        sent += static_cast<size_t>(r);
    }
}

void ScrcpyClient::SendTouch(int action, int64_t pointerId, float x, float y, float surfaceW, float surfaceH) {
    int w = videoW_;
    int h = videoH_;
    if (w <= 0 || h <= 0 || surfaceW <= 0 || surfaceH <= 0) return;
    // The XComponent keeps the video aspect ratio, so scaling is a plain ratio.
    int32_t vx = static_cast<int32_t>(x * w / surfaceW);
    int32_t vy = static_cast<int32_t>(y * h / surfaceH);
    bool up = action == 1;
    std::vector<uint8_t> msg;
    msg.reserve(32);
    msg.push_back(kInjectTouch);
    msg.push_back(static_cast<uint8_t>(action));
    Put64(msg, static_cast<uint64_t>(pointerId));
    Put32(msg, static_cast<uint32_t>(vx));
    Put32(msg, static_cast<uint32_t>(vy));
    Put16(msg, static_cast<uint16_t>(w));
    Put16(msg, static_cast<uint16_t>(h));
    Put16(msg, up ? 0 : 0xFFFF); // pressure
    Put32(msg, 0);               // action button
    Put32(msg, 0);               // buttons
    SendControl(msg);
}

void ScrcpyClient::SendKey(int keycode) {
    for (uint8_t action : {0, 1}) { // AKEY_EVENT_ACTION_DOWN, then UP
        std::vector<uint8_t> msg;
        msg.push_back(kInjectKeycode);
        msg.push_back(action);
        Put32(msg, static_cast<uint32_t>(keycode));
        Put32(msg, 0); // repeat
        Put32(msg, 0); // meta state
        SendControl(msg);
    }
}

void ScrcpyClient::SendText(const std::string &utf8) {
    if (utf8.empty()) return;
    size_t len = std::min(utf8.size(), kMaxTextLength);
    // Do not cut a UTF-8 sequence in the middle.
    while (len > 0 && len < utf8.size() && (static_cast<uint8_t>(utf8[len]) & 0xC0) == 0x80) {
        --len;
    }
    std::vector<uint8_t> msg;
    msg.push_back(kInjectText);
    Put32(msg, static_cast<uint32_t>(len));
    msg.insert(msg.end(), utf8.begin(), utf8.begin() + len);
    SendControl(msg);
}

void ScrcpyClient::RequestResize(int width, int height, int dpi) {
    {
        std::lock_guard<std::mutex> lock(controlMutex_);
        resizeW_ = width;
        resizeH_ = height;
        resizeDpi_ = dpi;
    }
    FlushResize();
}

void ScrcpyClient::FlushResize() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (!streaming_ || videoFd_ < 0 || resizeW_ <= 0 || resizeH_ <= 0) return;
    std::vector<uint8_t> msg = {'R', 'S', 'Z', '1'};
    Put16(msg, static_cast<uint16_t>(resizeW_));
    Put16(msg, static_cast<uint16_t>(resizeH_));
    Put16(msg, static_cast<uint16_t>(resizeDpi_));
    send(videoFd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    resizeW_ = resizeH_ = 0;
}
