#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <cxxopts.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <pjsua2.hpp>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(s) close(s)
#endif

using json = nlohmann::json;
using namespace pj;

std::atomic<bool> want_to_exit{false};
std::mutex log_mutex;

void log_json(const std::string &level, const std::string &event, const json &details = {}) {
    std::lock_guard<std::mutex> lock(log_mutex);
    json log_entry;
    log_entry["level"] = level;
    log_entry["event"] = event;
    log_entry["details"] = details;
    std::cout << log_entry.dump() << std::endl;
}

void signal_handler(int signum) {
    (void)signum;
    want_to_exit = true;
}

class SocketAudioBridge : public AudioMediaPort {
  private:
    std::thread socket_thread;
    std::atomic<bool> running;
    SOCKET server_sock;
    SOCKET client_sock;
    std::mutex socket_mutex;
    std::vector<char> up_buffer;
    std::mutex up_buffer_mutex;
    std::condition_variable up_buffer_cv;
    unsigned bytes_per_frame;
    size_t max_buffer_size_bytes;
    int underrun_log_counter = 0;
    const int UNDERRUN_LOG_INTERVAL = 100;

    void run_socket_server(int port) {
        SOCKET srv_sock_local = socket(AF_INET, SOCK_STREAM, 0);
        if (srv_sock_local == INVALID_SOCKET) {
            log_json("ERROR", "Socket creation failed");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            server_sock = srv_sock_local;
        }

        int opt = 1;
        setsockopt(srv_sock_local, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
        sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port);

        if (bind(srv_sock_local, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
            log_json("ERROR", "Socket bind failed", {{"port", port}});
            closesocket(srv_sock_local);
            return;
        }

        int actual_port = port;
        if (port == 0) {
            sockaddr_in local_address;
            socklen_t address_length = sizeof(local_address);
            if (getsockname(srv_sock_local, (struct sockaddr *)&local_address, &address_length) == 0) {
                actual_port = ntohs(local_address.sin_port);
            }
        }

        if (listen(srv_sock_local, 1) == SOCKET_ERROR) {
            log_json("ERROR", "Socket listen failed");
            closesocket(srv_sock_local);
            return;
        }

        log_json("INFO", "Socket server started", {{"port", actual_port}, {"type", (port == 0 ? "auto" : "fixed")}});

        while (running) {
            log_json("INFO", "Server waiting for new connection...");
            SOCKET accepted_sock = accept(srv_sock_local, nullptr, nullptr);

            if (accepted_sock == INVALID_SOCKET) {
                if (!running) {
                    log_json("INFO", "Accept call interrupted for shutdown.");
                } else {
                    log_json("ERROR", "Socket accept failed while server was running.");
                }
                break;
            }

            log_json("INFO", "Socket client connected");
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                client_sock = accepted_sock;
            }

            while (running) {
                char recv_buf[4096];
                int bytes_received = recv(client_sock, recv_buf, sizeof(recv_buf), 0);
                if (bytes_received > 0) {
                    std::lock_guard<std::mutex> lock(up_buffer_mutex);
                    if (max_buffer_size_bytes > 0 && up_buffer.size() + bytes_received > max_buffer_size_bytes) {
                        size_t overflow = (up_buffer.size() + bytes_received) - max_buffer_size_bytes;
                        size_t to_discard = std::min(up_buffer.size(), overflow);
                        up_buffer.erase(up_buffer.begin(), up_buffer.begin() + to_discard);
                        log_json("WARN", "Jitter buffer overflow", {{"discarded_bytes", to_discard}});
                    }
                    up_buffer.insert(up_buffer.end(), recv_buf, recv_buf + bytes_received);
                    up_buffer_cv.notify_one();
                } else {
                    log_json("INFO", "Socket client disconnected.");
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                if (client_sock != INVALID_SOCKET) {
                    closesocket(client_sock);
                    client_sock = INVALID_SOCKET;
                }
            }
        }

        log_json("INFO", "Socket server thread shutting down.");
        closesocket(srv_sock_local);
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            server_sock = INVALID_SOCKET;
        }
    }

  public:
    SocketAudioBridge() : running(false), server_sock(INVALID_SOCKET), client_sock(INVALID_SOCKET), bytes_per_frame(0), max_buffer_size_bytes(0) {}
    ~SocketAudioBridge() { stop(); }

    void set_buffer_parameters(unsigned pjsip_bytes_per_frame, size_t max_buffer_bytes) {
        bytes_per_frame = pjsip_bytes_per_frame;
        max_buffer_size_bytes = max_buffer_bytes;
    }

    void createPort(EpConfig &ep_cfg) {
        MediaFormatAudio fmt;
        const MediaConfig &med_cfg = ep_cfg.medConfig;
        fmt.clockRate = med_cfg.clockRate;
        fmt.channelCount = med_cfg.channelCount;
        fmt.bitsPerSample = 16;
        fmt.frameTimeUsec = med_cfg.audioFramePtime * 1000;
        AudioMediaPort::createPort("socket_audio_bridge", fmt);
        log_json("INFO", "SocketAudioBridge media port created", {{"bytes_per_frame", bytes_per_frame}, {"max_jitter_buffer_bytes", max_buffer_size_bytes}});
    }

    void start(int port) {
        if (!running) {
            running = true;
            socket_thread = std::thread(&SocketAudioBridge::run_socket_server, this, port);
        }
    }

    void stop() {
        if (running.exchange(false)) {
            SOCKET server_to_close = INVALID_SOCKET;
            SOCKET client_to_close = INVALID_SOCKET;

            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                server_to_close = server_sock;
                client_to_close = client_sock;
            }

            if (server_to_close != INVALID_SOCKET) {
#ifdef _WIN32
                closesocket(server_to_close);
#else
                shutdown(server_to_close, SHUT_RDWR);
                closesocket(server_to_close);
#endif
            }
            if (client_to_close != INVALID_SOCKET) {
                closesocket(client_to_close);
            }

            up_buffer_cv.notify_all();
            if (socket_thread.joinable()) {
                socket_thread.join();
            }
        }
    }

    void disconnect_client() {
        std::lock_guard<std::mutex> lock(socket_mutex);
        if (client_sock != INVALID_SOCKET) {
            log_json("INFO", "Actively disconnecting TCP client.");
            closesocket(client_sock);
            client_sock = INVALID_SOCKET;
        }
    }

    virtual void onFrameRequested(MediaFrame &frame) override {
        frame.buf.resize(bytes_per_frame);
        bool underrun = false;

        {
            std::lock_guard<std::mutex> lock(up_buffer_mutex);
            if (up_buffer.size() < bytes_per_frame) {
                underrun = true;
            } else {
                underrun_log_counter = 0;
                memcpy(frame.buf.data(), up_buffer.data(), bytes_per_frame);
                up_buffer.erase(up_buffer.begin(), up_buffer.begin() + bytes_per_frame);
            }
        }

        if (underrun) {
            if (++underrun_log_counter >= UNDERRUN_LOG_INTERVAL) {
                log_json("WARN", "Jitter buffer underrun", {{"interval_count", UNDERRUN_LOG_INTERVAL}});
                underrun_log_counter = 0;
            }
            memset(frame.buf.data(), 0, bytes_per_frame);
        }

        frame.type = PJMEDIA_FRAME_TYPE_AUDIO;
        frame.size = bytes_per_frame;
    }

    virtual void onFrameReceived(MediaFrame &frame) override {
        std::lock_guard<std::mutex> lock(socket_mutex);
        if (client_sock != INVALID_SOCKET) {
#ifdef _WIN32
            int bytes_sent = send(client_sock, (const char *)frame.buf.data(), frame.size, 0);
            if (bytes_sent == SOCKET_ERROR) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    log_json("WARN", "Socket send error", {{ "error_code", WSAGetLastError() }});
                }
            }
#else
            int bytes_sent = send(client_sock, (const char *)frame.buf.data(), frame.size, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (bytes_sent < 0) {
                if (errno != EWOULDBLOCK && errno != EAGAIN) {
                    log_json("WARN", "Socket send error", {{"errno", errno}});
                }
            }
#endif
        }
    }
};

class MyCall;
class MyAccount : public Account {
  public:
    std::unique_ptr<MyCall> call = nullptr;
    std::atomic<bool> is_registered{false};

    MyAccount() = default;
    ~MyAccount() = default;

    void makeCall(const std::string &destUri, SocketAudioBridge *bridge);

    virtual void onRegState(OnRegStateParam &prm) override {
        json details = {{"code", prm.code}, {"reason", prm.reason}};
        if (prm.code == PJSIP_SC_OK) {
            if (prm.expiration == 0) {
                log_json("INFO", "SIP unregistration successful", details);
                is_registered = false;
            } else {
                log_json("INFO", "SIP registration successful", details);
                is_registered = true;
            }
        } else {
            log_json("WARN", "SIP registration update", details);
            is_registered = false;
        }
    }
};

class MyCall : public Call {
  private:
    SocketAudioBridge *audio_bridge;
    MyAccount &my_acc;

  public:
    MyCall(MyAccount &acc, SocketAudioBridge *bridge, int call_id = PJSUA_INVALID_ID) : Call(acc, call_id), audio_bridge(bridge), my_acc(acc) {}

    ~MyCall() {
        try {
            if (audio_bridge && getInfo().state != PJSIP_INV_STATE_DISCONNECTED) {
                AudioMedia call_aud_med = getAudioMedia(-1);
                audio_bridge->stopTransmit(call_aud_med);
                call_aud_med.stopTransmit(*audio_bridge);
            }
        } catch (...) {
        }
    }

    virtual void onCallState(OnCallStateParam &prm) override {
        CallInfo ci;
        try {
            ci = getInfo();
        } catch (...) {
            return;
        }
        log_json("INFO", "Call state changed", {{"state", ci.stateText}});
        if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
            try {
                AudioMedia call_aud_med = getAudioMedia(-1);
                audio_bridge->startTransmit(call_aud_med);
                call_aud_med.startTransmit(*audio_bridge);
                log_json("INFO", "Bidirectional audio bridge established");
            } catch (Error &err) {
                log_json("ERROR", "Failed to connect audio bridge", {{"details", err.info()}});
            }
        } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
            log_json("INFO", "Call disconnected", {{"reason", ci.lastReason}});
            if (audio_bridge) {
                audio_bridge->disconnect_client();
            }
            my_acc.call.reset();
            want_to_exit = true;
        }
    }
};

inline void MyAccount::makeCall(const std::string &destUri, SocketAudioBridge *bridge) {
    call = std::make_unique<MyCall>(*this, bridge);
    CallOpParam prm(true);
    prm.opt.audioCount = 1;
    prm.opt.videoCount = 0;
    try {
        log_json("INFO", "Making SIP call", {{"destination", destUri}});
        call->makeCall(destUri, prm);
    } catch (Error &err) {
        log_json("ERROR", "Call failed on initiation", {{"details", err.info()}});
        call.reset();
    }
}

int main(int argc, char *argv[]) {
    cxxopts::Options options(argv[0], "A PJSIP to Raw Audio TCP Socket Bridge");
    auto opts = options.add_options();

    opts("u,user", "SIP account username", cxxopts::value<std::string>());
    opts("p,pass", "SIP account password", cxxopts::value<std::string>());
    opts("s,server", "SIP server domain", cxxopts::value<std::string>());
    opts("t,uri", "Target SIP URI to call", cxxopts::value<std::string>());
    opts("port", "TCP socket port (0 for auto)", cxxopts::value<int>()->default_value("0"));
    opts("rate", "Audio clock rate (e.g., 8000)", cxxopts::value<int>()->default_value("8000"));
    opts("c,codecs", "Comma-separated list of codecs", cxxopts::value<std::string>()->default_value("PCMU/8000,PCMA/8000"));
    opts("j,jbuf-bytes", "Jitter buffer size in bytes", cxxopts::value<size_t>()->default_value("512"));
    opts("l,pjsip-log", "PJSIP native log level", cxxopts::value<int>()->default_value("0"));
    opts("h,help", "Print usage");
    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        std::cerr << options.help() << std::endl;
        return 1;
    }

    if (result.count("help") || !result.count("user") || !result.count("pass") || !result.count("server") || !result.count("uri")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    auto user = result["user"].as<std::string>();
    auto password = result["pass"].as<std::string>();
    auto server = result["server"].as<std::string>();
    auto targetUri = result["uri"].as<std::string>();
    auto socket_port = result["port"].as<int>();
    auto clock_rate = result["rate"].as<int>();
    auto codecs_str = result["codecs"].as<std::string>();
    auto jbuf_bytes = result["jbuf-bytes"].as<size_t>();
    auto pjsip_log_level = result["pjsip-log"].as<int>();

    log_json("INFO", "Application starting", {{"file", argv[0]}});

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log_json("ERROR", "WSAStartup failed");
        return 1;
    }
#endif

    std::unique_ptr<Endpoint> ep;
    std::unique_ptr<SocketAudioBridge> audio_bridge;
    std::unique_ptr<MyAccount> acc;

    try {
        ep = std::make_unique<Endpoint>();
        ep->libCreate();

        EpConfig ep_cfg;
        ep_cfg.logConfig.level = pjsip_log_level;
        ep_cfg.logConfig.consoleLevel = pjsip_log_level;
        ep_cfg.medConfig.noVad = PJ_TRUE;
        ep_cfg.medConfig.clockRate = clock_rate;
        ep_cfg.medConfig.ecOptions = 0;
        ep_cfg.medConfig.ecTailLen = 0;
        ep->libInit(ep_cfg);

        log_json("INFO", "PJSIP endpoint initialized", {{"pjsip_log_level", pjsip_log_level}});

        ep->audDevManager().setNullDev();
        log_json("INFO", "Physical sound device disabled");

        try {
            std::vector<CodecInfo> codecs = ep->codecEnum2();
            for (auto &codec : codecs) {
                ep->codecSetPriority(codec.codecId, 0);
            }
            std::stringstream ss(codecs_str);
            std::string codec_id;
            int priority = 255;
            json enabled_codecs = json::array();
            while (std::getline(ss, codec_id, ',')) {
                if (priority > 0) {
                    try {
                        ep->codecSetPriority(codec_id, priority--);
                        enabled_codecs.push_back(codec_id);
                    } catch (Error &err) {
                        log_json("WARN", "Could not set priority for codec", {{"codec", codec_id}, {"error", err.info()}});
                    }
                }
            }
            log_json("INFO", "Codec priorities adjusted", {{"enabled", enabled_codecs}});
        } catch (Error &err) {
            log_json("ERROR", "Codec adjustment failed", {{"error", err.info()}});
        }

        TransportConfig tcfg;
        tcfg.port = 6060;
        ep->transportCreate(PJSIP_TRANSPORT_UDP, tcfg);

        ep->libStart();
        log_json("INFO", "PJSIP library started");

        const MediaConfig &med_cfg = ep_cfg.medConfig;
        audio_bridge = std::make_unique<SocketAudioBridge>();

        unsigned bytes_per_frame = (med_cfg.clockRate * med_cfg.audioFramePtime / 1000) * med_cfg.channelCount * sizeof(short);
        audio_bridge->set_buffer_parameters(bytes_per_frame, jbuf_bytes);

        audio_bridge->createPort(ep_cfg);
        audio_bridge->start(socket_port);

        AccountConfig acfg;
        acfg.idUri = "sip:" + user + "@" + server;
        acfg.regConfig.registrarUri = "sip:" + server;
        AuthCredInfo cred("digest", "*", user, 0, password);
        acfg.sipConfig.authCreds.push_back(cred);
        acfg.sipConfig.proxies.push_back("sip:" + server + ";transport=udp;lr");

        acc = std::make_unique<MyAccount>();
        acc->create(acfg);
        log_json("INFO", "SIP account created. Waiting for registration...");

        bool registration_success = false;
        for (int i = 0; i < 100; ++i) {
            if (want_to_exit)
                break;
            if (acc && acc->is_registered) {
                registration_success = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (registration_success && !want_to_exit) {
            acc->makeCall(targetUri, audio_bridge.get());
            while (!want_to_exit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        } else if (!registration_success && !want_to_exit) {
            log_json("ERROR", "SIP registration failed or timed out");
        }

        if (want_to_exit) {
            log_json("INFO", "Exit signal detected, beginning shutdown...");
        }

    } catch (Error &err) {
        log_json("ERROR", "A high-level exception occurred", {{"details", err.info()}});
        want_to_exit = true;
    }

    log_json("INFO", "Starting shutdown process");
    if (acc && acc->call) {
        log_json("INFO", "Hanging up active call");
        CallOpParam prm;
        try {
            acc->call->hangup(prm);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } catch (...) {
            log_json("WARN", "Exception while hanging up call");
        }
    }

    if (audio_bridge) {
        log_json("INFO", "Stopping audio bridge");
        audio_bridge->stop();
    }

    if (ep) {
        log_json("INFO", "Destroying PJSIP endpoint");
        try {
            ep->libDestroy();
        } catch (...) {
            log_json("WARN", "Exception while destroying PJSIP endpoint");
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
    log_json("INFO", "Shutdown complete");
    return 0;
}