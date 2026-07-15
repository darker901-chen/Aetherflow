// SRT/MPEG-TS live output stage. See the header for the threading contract.
//
// Winsock note: winsock2.h must precede any windows.h in this TU, which is
// why the PrintShareUrls helper lives here instead of main.cpp (main.cpp
// includes <Windows.h> without WIN32_LEAN_AND_MEAN).

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "AetherFlow/streaming/SrtStreamOutput.h"
#include "AetherFlow/streaming/AnnexB.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include <chrono>
#include <cstring>
#include <iostream>

namespace AetherFlow {

namespace {

// Bounded-queue caps. ~8 MiB / 256 AUs is several seconds of 1080p30 screen
// content; when a viewer stalls (or nobody is connected) the oldest AUs are
// dropped so encoder-side memory stays flat.
constexpr size_t kMaxQueueBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxQueueAus = 256;

std::string AvErrorText(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace

SrtStreamOutput::SrtStreamOutput(const Options& options) : m_options(options) {}

SrtStreamOutput::~SrtStreamOutput() {
    Stop();
}

std::string SrtStreamOutput::ListenUrl() const {
    return "srt://0.0.0.0:" + std::to_string(m_options.port);
}

bool SrtStreamOutput::Start() {
    if (m_worker.joinable()) {
        return true;
    }
    // Keep libavformat quiet unless something is actually wrong; our own
    // [SRT] lines carry the state the run artifacts need.
    av_log_set_level(AV_LOG_ERROR);
    avformat_network_init();

    bool srtAvailable = false;
    void* opaque = nullptr;
    while (const char* name = avio_enum_protocols(&opaque, 1)) {
        if (std::strcmp(name, "srt") == 0) {
            srtAvailable = true;
            break;
        }
    }
    if (!srtAvailable) {
        std::cerr << "[SRT] ERROR: the linked FFmpeg has no 'srt' protocol; "
                     "re-vendor with tools/fetch_ffmpeg.py (see third_party/ffmpeg/SOURCE.md).\n";
        return false;
    }

    m_stop.store(false);
    m_worker = std::thread(&SrtStreamOutput::WorkerLoop, this);
    return true;
}

void SrtStreamOutput::Stop() {
    m_stop.store(true);
    m_queueCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void SrtStreamOutput::OnEncodedAccessUnit(const EncodedAccessUnit& au) {
    if (m_stop.load(std::memory_order_relaxed)) {
        return;
    }
    if (!au.data || au.size == 0) {
        return;
    }
    QueuedAu queued;
    queued.data.assign(au.data, au.data + au.size);
    queued.pts90k = au.pts90k;
    queued.keyframe = au.keyframe;

    // Harvest parameter sets on the enqueue side so the cache survives the
    // connect-time queue clear (the first IDR is the only AU guaranteed to
    // carry SPS/PPS in-band). Keyframes arrive once per GOP, so this scan is
    // off the per-frame path.
    std::vector<uint8_t> spsPps;
    const bool haveSpsPps =
        au.keyframe && annexb::ExtractSpsPps(au.data, au.size, &spsPps);

    uint64_t droppedNow = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (haveSpsPps) {
            m_cachedSpsPps = std::move(spsPps);
        }
        m_queueBytes += queued.data.size();
        m_queue.push_back(std::move(queued));
        while ((m_queueBytes > kMaxQueueBytes || m_queue.size() > kMaxQueueAus) &&
               m_queue.size() > 1) {
            m_queueBytes -= m_queue.front().data.size();
            m_queue.pop_front();
            ++droppedNow;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.enqueued;
        m_stats.droppedQueueFull += droppedNow;
    }
    m_queueCv.notify_one();
}

SrtStreamOutput::Stats SrtStreamOutput::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

int SrtStreamOutput::InterruptCb(void* opaque) {
    auto* self = static_cast<SrtStreamOutput*>(opaque);
    return self->m_stop.load(std::memory_order_relaxed) ? 1 : 0;
}

void SrtStreamOutput::WorkerLoop() {
    while (!m_stop.load()) {
        const bool servedViewer = OpenAndServeOnce();
        if (m_stop.load()) {
            break;
        }
        // Brief backoff before re-listening; longer after an open failure
        // (e.g. port already in use) so the log does not spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(servedViewer ? 50 : 500));
    }
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_stats.listening = false;
    m_stats.clientConnected = false;
}

bool SrtStreamOutput::OpenAndServeOnce() {
    AVFormatContext* ctx = nullptr;
    if (avformat_alloc_output_context2(&ctx, nullptr, "mpegts", nullptr) < 0 || !ctx) {
        std::cerr << "[SRT] failed to allocate the mpegts muxer context.\n";
        return false;
    }
    ctx->interrupt_callback.callback = &SrtStreamOutput::InterruptCb;
    ctx->interrupt_callback.opaque = this;
    // Flush the TS packetizer to the socket after every written frame; we are
    // latency-bound, not throughput-bound.
    ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    AVStream* stream = avformat_new_stream(ctx, nullptr);
    if (!stream) {
        std::cerr << "[SRT] failed to create the output stream.\n";
        avformat_free_context(ctx);
        return false;
    }
    stream->time_base = AVRational{1, 90000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_H264;
    stream->codecpar->width = m_options.width;
    stream->codecpar->height = m_options.height;

    const std::string url = ListenUrl();
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "mode", "listener", 0);
    av_dict_set(&opts, "transtype", "live", 0);
    // FFmpeg's srt "latency" option is in MICROseconds; the CLI/UI unit is ms.
    av_dict_set_int(&opts, "latency", static_cast<int64_t>(m_options.latencyMs) * 1000, 0);
    if (!m_options.passphrase.empty()) {
        av_dict_set(&opts, "passphrase", m_options.passphrase.c_str(), 0);
        av_dict_set_int(&opts, "pbkeylen", 16, 0);  // AES-128
    }

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.listening = true;
        m_stats.clientConnected = false;
    }
    std::cout << "[SRT] listening on " << url << " (waiting for a viewer)\n";

    // Blocks until a caller connects; InterruptCb aborts it on Stop().
    const int openRc = avio_open2(&ctx->pb, url.c_str(), AVIO_FLAG_WRITE,
                                  &ctx->interrupt_callback, &opts);
    av_dict_free(&opts);
    if (openRc < 0) {
        if (!m_stop.load()) {
            std::cerr << "[SRT] listener open failed: " << AvErrorText(openRc) << "\n";
        }
        avformat_free_context(ctx);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        ++m_stats.connections;
        m_stats.clientConnected = true;
    }
    std::cout << "[SRT] viewer connected\n";

    // Serve fresh content only: whatever queued while nobody watched is stale.
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_queueBytes = 0;
    }

    bool viewerOk = true;
    bool headerWritten = false;
    const int headerRc = avformat_write_header(ctx, nullptr);
    if (headerRc < 0) {
        std::cerr << "[SRT] mpegts header write failed: " << AvErrorText(headerRc) << "\n";
        viewerOk = false;
    } else {
        headerWritten = true;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        viewerOk = false;
    }

    bool sentFirstKeyframe = false;
    uint64_t basePts90k = 0;
    uint64_t awaitingKeyframeDrops = 0;
    std::vector<uint8_t> sendBuffer;

    while (viewerOk && !m_stop.load()) {
        QueuedAu au;
        std::vector<uint8_t> spsPpsForJoin;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
            if (m_stop.load()) {
                break;
            }
            au = std::move(m_queue.front());
            m_queue.pop_front();
            m_queueBytes -= au.data.size();
            if (au.keyframe) {
                spsPpsForJoin = m_cachedSpsPps;  // filled on the enqueue side
            }
        }

        if (!sentFirstKeyframe) {
            if (!au.keyframe) {
                ++awaitingKeyframeDrops;
                continue;  // a decoder cannot enter mid-GOP
            }
            sentFirstKeyframe = true;
            basePts90k = au.pts90k;
        }

        const uint8_t* data = au.data.data();
        size_t size = au.data.size();
        if (au.keyframe && !spsPpsForJoin.empty() &&
            !annexb::ContainsSpsPps(data, size)) {
            sendBuffer.clear();
            sendBuffer.reserve(spsPpsForJoin.size() + size);
            sendBuffer.insert(sendBuffer.end(), spsPpsForJoin.begin(), spsPpsForJoin.end());
            sendBuffer.insert(sendBuffer.end(), data, data + size);
            data = sendBuffer.data();
            size = sendBuffer.size();
        }

        packet->data = const_cast<uint8_t*>(data);
        packet->size = static_cast<int>(size);
        const int64_t pts =
            (au.pts90k >= basePts90k) ? static_cast<int64_t>(au.pts90k - basePts90k) : 0;
        packet->pts = pts;
        packet->dts = pts;  // frameIntervalP == 1 / no B-frames on both backends
        packet->duration = (m_options.fps > 0) ? (90000 / m_options.fps) : 0;
        packet->stream_index = stream->index;
        packet->flags = au.keyframe ? AV_PKT_FLAG_KEY : 0;
        if (stream->time_base.num != 1 || stream->time_base.den != 90000) {
            av_packet_rescale_ts(packet, AVRational{1, 90000}, stream->time_base);
        }

        const int writeRc = av_write_frame(ctx, packet);
        if (writeRc < 0) {
            std::cout << "[SRT] viewer disconnected (" << AvErrorText(writeRc)
                      << "); returning to listen mode\n";
            viewerOk = false;
        } else {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            ++m_stats.sent;
            m_stats.bytesSent += size;
        }
    }

    if (packet) {
        av_packet_free(&packet);
    }
    if (headerWritten && viewerOk) {
        // Clean shutdown with the viewer still attached (run ended).
        av_write_trailer(ctx);
    }
    if (ctx->pb) {
        avio_closep(&ctx->pb);
    }
    avformat_free_context(ctx);

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.clientConnected = false;
        m_stats.droppedAwaitingKeyframe += awaitingKeyframeDrops;
    }
    return true;
}

std::vector<std::string> SrtStreamOutput::ShareUrls(int port) {
    std::vector<std::string> urls;
    urls.push_back("srt://127.0.0.1:" + std::to_string(port));
#if defined(_WIN32)
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return urls;
    }
    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &results) == 0 && results) {
            for (addrinfo* entry = results; entry; entry = entry->ai_next) {
                char ip[INET_ADDRSTRLEN] = {};
                auto* addr = reinterpret_cast<sockaddr_in*>(entry->ai_addr);
                if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) &&
                    std::strcmp(ip, "127.0.0.1") != 0) {
                    urls.push_back("srt://" + std::string(ip) + ":" + std::to_string(port));
                }
            }
            freeaddrinfo(results);
        }
    }
    WSACleanup();
#endif
    return urls;
}

void SrtStreamOutput::PrintShareUrls(int port) {
    const auto urls = ShareUrls(port);
    for (size_t i = 0; i < urls.size(); ++i) {
        if (i == 0) {
            std::cout << "[SRT] local viewer URL: " << urls[i] << "\n";
        } else {
            std::cout << "[SRT] LAN viewer URL:   " << urls[i]
                      << "  (paste into VLC / ffplay on any LAN device)\n";
        }
    }
}

}  // namespace AetherFlow
