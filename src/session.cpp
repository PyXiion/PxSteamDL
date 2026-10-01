// SPDX-License-Identifier: LGPL-3.0-or-later
#include "session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>

#include "cm_packet.hpp"
#include "http.hpp"
#include "proto.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace pxsteamdl::detail {

namespace {

constexpr char kCmListUrl[] = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0";
// CM endpoints tried before giving up.
constexpr std::size_t kMaxCmAttempts = 5;
constexpr std::uint64_t kAnonymousSteamId = 0x01A0000000000000ULL;

constexpr long kConnectTimeoutSeconds = 10;
constexpr auto kLogonTimeout = std::chrono::seconds(10);
constexpr auto kCallTimeout = std::chrono::seconds(30);
constexpr int kSendPollTimeoutMs = 5000;
// How long the reader waits for data before it checks whether to stop.
constexpr int kReadPollTimeoutMs = 50;
constexpr int kDefaultHeartbeatSeconds = 9;
constexpr int kMinHeartbeatSeconds = 1;
constexpr int kMaxHeartbeatSeconds = 60;

// CMsgClientLogon field numbers and the values sent for an anonymous logon.
namespace logon_field {
constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kCellId = 3;
constexpr std::uint32_t kClientLanguage = 6;
constexpr std::uint32_t kClientOsType = 7;
}  // namespace logon_field
constexpr std::uint32_t kProtocolVersion = 65581;
// EOSType Linux 6.x (-184) as an unsigned 32-bit value.
constexpr std::uint32_t kOsTypeLinux6x = 4294967112u;

// CMsgClientLogonResponse field numbers.
namespace logon_response_field {
constexpr std::uint32_t kEResult = 1;
constexpr std::uint32_t kHeartbeatSeconds = 3;
}  // namespace logon_response_field

// Waits up to timeout_ms for the socket to become readable (or writable); returns false on timeout or error.
bool WaitSocket(curl_socket_t socket, bool write, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD entry{socket, static_cast<SHORT>(write ? POLLOUT : POLLIN), 0};
    return WSAPoll(&entry, 1, timeout_ms) > 0;
#else
    pollfd entry{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&entry, 1, timeout_ms) > 0;
#endif
}

curl_socket_t ActiveSocket(CURL* curl) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
    return socket;
}

enum class ReceiveStatus {
    kMessage,  // a complete message was moved into message
    kMore,     // read again right away
    kAgain,    // nothing buffered: wait for the socket
    kClosed,
};

// One non-blocking read from a CONNECT_ONLY WebSocket. Data frames are reassembled in partial; a completed message
// is moved into message. Control frames (curl answers pings itself) are skipped.
ReceiveStatus Receive(CURL* curl, Bytes& partial, Bytes& message) {
    std::uint8_t buffer[65536];
    std::size_t read = 0;
    const curl_ws_frame* meta = nullptr;
    CURLcode status = curl_ws_recv(curl, buffer, sizeof buffer, &read, &meta);
    if (status == CURLE_AGAIN) return ReceiveStatus::kAgain;
    if (status != CURLE_OK || !meta || (meta->flags & CURLWS_CLOSE)) return ReceiveStatus::kClosed;
    if (!(meta->flags & (CURLWS_BINARY | CURLWS_TEXT))) return ReceiveStatus::kMore;
    partial.insert(partial.end(), buffer, buffer + read);
    if (meta->bytesleft != 0 || (meta->flags & CURLWS_CONT)) return ReceiveStatus::kMore;
    message = std::move(partial);
    partial.clear();
    return ReceiveStatus::kMessage;
}

std::vector<std::string> FetchCmEndpoints() {
    HttpResponse response = HttpRequest(kCmListUrl);
    if (response.status != 200) Fail("GetCMListForConnect: HTTP " + std::to_string(response.status));
    auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
    const auto& servers = root.at("response").at("serverlist");
    if (!servers.is_array()) Fail("GetCMListForConnect: invalid serverlist");

    std::vector<std::string> endpoints;
    for (const auto& server : servers) {
        if (server.value("type", "") != "websockets" || !server.contains("endpoint") ||
            !server["endpoint"].is_string()) {
            continue;
        }
        auto endpoint = server["endpoint"].get<std::string>();
        if (!endpoint.empty()) endpoints.push_back(std::move(endpoint));
    }
    if (endpoints.empty()) Fail("No websocket CM endpoints found");
    return endpoints;
}

Bytes LogonBody() {
    return Concat({
        EncodeUint(logon_field::kProtocolVersion, kProtocolVersion),
        EncodeUint(logon_field::kCellId, 0),
        EncodeString(logon_field::kClientLanguage, "english"),
        EncodeUint(logon_field::kClientOsType, kOsTypeLinux6x),
    });
}

struct LogonResult {
    std::int64_t eresult = 0;
    int heartbeat_seconds = kDefaultHeartbeatSeconds;
};

LogonResult ParseLogonResponse(const Packet& packet) {
    LogonResult result;
    result.eresult = packet.eresult;
    ProtoReader reader(packet.body);
    while (auto field = reader.Next()) {
        if (field->Is(logon_response_field::kEResult, WireType::kVarint)) {
            result.eresult = static_cast<int>(field->integer);
        } else if (field->Is(logon_response_field::kHeartbeatSeconds, WireType::kVarint)) {
            result.heartbeat_seconds = static_cast<int>(field->integer);
        }
    }
    return result;
}

}  // namespace

class Session::Impl {
public:
    ~Impl() {
        std::lock_guard lock(connect_mutex_);
        Close();
    }

    void Connect() {
        std::lock_guard lock(connect_mutex_);
        if (connected_) return;
        Close();  // joins the threads of a dropped connection
        Open();
    }

    // Sends a packet as a new job and waits for the packet that answers it.
    Packet Call(std::uint32_t emsg, std::string_view job_name, ByteSpan body) {
        std::uint64_t jobid = next_job_id_++;
        std::future<Packet> future;
        {
            // Checked under jobs_mutex_ so the job cannot be registered after the reader failed the pending ones.
            std::lock_guard lock(jobs_mutex_);
            if (!connected_) Fail("Session not connected");
            future = pending_jobs_[jobid].get_future();
        }
        auto forget = [&] {
            std::lock_guard lock(jobs_mutex_);
            pending_jobs_.erase(jobid);
        };
        try {
            Packet request = SessionPacket(emsg);
            request.jobid_source = jobid;
            request.target_job_name = job_name;
            request.body.assign(body.begin(), body.end());
            SendPacket(SerializePacket(request));
        } catch (...) {
            forget();
            throw;
        }
        if (future.wait_for(kCallTimeout) == std::future_status::timeout) {
            forget();
            Fail("Steam CM request timed out (EMsg " + std::to_string(emsg) + " " + std::string(job_name) + ")");
        }
        return future.get();
    }

private:
    // A packet outside any job.
    Packet SessionPacket(std::uint32_t emsg) const {
        Packet packet;
        packet.emsg = emsg;
        packet.steamid = steamid_;
        packet.sessionid = sessionid_;
        return packet;
    }

    // Tries the CM endpoints in turn. Requires connect_mutex_ and a closed session.
    void Open() {
        std::vector<std::string> endpoints = FetchCmEndpoints();
        std::string last_error = "no endpoint reachable";
        for (std::size_t i = 0; i < std::min(endpoints.size(), kMaxCmAttempts); ++i) {
            try {
                if (Logon("wss://" + endpoints[i] + "/cmsocket/")) return;
                last_error = "no logon response";
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            std::lock_guard lock(io_mutex_);
            if (curl_) curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
        Fail("Failed to connect to Steam CM: " + last_error);
    }

    // Logs off (if still connected), stops the threads and releases the socket. Requires connect_mutex_.
    void Close() {
        bool was_connected = MarkDisconnected("Session disconnected");
        StopThreads();
        if (was_connected) {
            try {
                SendPacket(SerializePacket(SessionPacket(emsg::kClientLogOff)));
            } catch (...) {
                // The connection is going away anyway.
            }
        }
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
        if (reader_thread_.joinable()) reader_thread_.join();

        std::lock_guard lock(io_mutex_);
        if (curl_) curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }

    void StopThreads() {
        {
            std::lock_guard lock(heartbeat_mutex_);  // no lost wakeup between the heartbeat's check and wait
            stopping_ = true;
        }
        heartbeat_cv_.notify_all();
    }

    // Stops accepting calls and fails the ones in flight; returns whether the session was connected.
    bool MarkDisconnected(const char* reason) {
        std::lock_guard lock(jobs_mutex_);
        bool was_connected = connected_.exchange(false);
        for (auto& [id, promise] : pending_jobs_) {
            promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
        }
        pending_jobs_.clear();
        return was_connected;
    }

    // Opens the socket and logs on anonymously; on success starts the reader and heartbeat threads.
    // Returns false on a connection failure or timeout, throws on a rejected logon.
    bool Logon(const std::string& url) {
        CURL* handle = curl_easy_init();
        if (!handle) Fail("curl_easy_init failed");
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        SetCaBundle(handle);
        curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        {
            std::lock_guard lock(io_mutex_);
            curl_ = handle;  // owned (and cleaned up on failure) by Open()
        }
        if (curl_easy_perform(handle) != CURLE_OK) return false;

        Packet logon;
        logon.emsg = emsg::kClientLogon;
        logon.steamid = kAnonymousSteamId;
        logon.body = LogonBody();
        SendPacket(SerializePacket(logon));
        std::optional<Packet> response = AwaitLogonResponse(handle);
        if (!response) return false;
        LogonResult result = ParseLogonResponse(*response);
        if (result.eresult != kEResultOk) Fail("Logon failed with EResult " + std::to_string(result.eresult));

        steamid_ = response->steamid;
        sessionid_ = response->sessionid;
        stopping_ = false;
        connected_ = true;
        reader_thread_ = std::thread(&Impl::ReaderLoop, this);
        heartbeat_thread_ =
            std::thread(&Impl::HeartbeatLoop, this,
                        std::clamp(result.heartbeat_seconds, kMinHeartbeatSeconds, kMaxHeartbeatSeconds));
        return true;
    }

    // Reads until the logon response arrives; nullopt if the socket closes or the logon times out. No other thread
    // uses the handle yet, so it is read synchronously.
    static std::optional<Packet> AwaitLogonResponse(CURL* handle) {
        Bytes partial;
        Bytes message;
        auto deadline = std::chrono::steady_clock::now() + kLogonTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            ReceiveStatus status = Receive(handle, partial, message);
            if (status == ReceiveStatus::kClosed) return std::nullopt;
            if (status == ReceiveStatus::kMore) continue;
            if (status == ReceiveStatus::kAgain) {
                WaitSocket(ActiveSocket(handle), false, kReadPollTimeoutMs);
                continue;
            }
            for (Packet& packet : ParseMessage(message)) {
                if (packet.emsg == emsg::kClientLogonResponse) return std::move(packet);
            }
        }
        return std::nullopt;
    }

    void SendPacket(ByteSpan data) {
        std::lock_guard lock(io_mutex_);
        if (!curl_) Fail("Session not connected");
        curl_socket_t socket = ActiveSocket(curl_);
        for (std::size_t offset = 0; offset < data.size();) {
            std::size_t sent = 0;
            CURLcode status = curl_ws_send(curl_, data.data() + offset, data.size() - offset, &sent, 0, CURLWS_BINARY);
            if (status == CURLE_AGAIN) {
                if (socket == CURL_SOCKET_BAD) Fail("curl socket unavailable for send");
                if (!WaitSocket(socket, true, kSendPollTimeoutMs)) Fail("curl_ws_send poll timeout");
                continue;
            }
            if (status != CURLE_OK) Fail(std::string("curl_ws_send failed: ") + curl_easy_strerror(status));
            offset += sent;
        }
    }

    // Hands each packet of a message to the call waiting for it.
    void Dispatch(ByteSpan message) {
        std::vector<Packet> packets;
        try {
            packets = ParseMessage(message);
        } catch (const std::exception&) {
            return;  // malformed message: nothing to route
        }
        for (Packet& packet : packets) {
            if (packet.emsg == emsg::kClientLoggedOff) StopThreads();
            if (packet.jobid_target == kNoJob) continue;
            std::promise<Packet> promise;
            {
                std::lock_guard lock(jobs_mutex_);
                auto it = pending_jobs_.find(packet.jobid_target);
                if (it == pending_jobs_.end()) continue;
                promise = std::move(it->second);
                pending_jobs_.erase(it);
            }
            promise.set_value(std::move(packet));
        }
    }

    void ReaderLoop() {
        Bytes partial;
        Bytes message;
        while (!stopping_) {
            ReceiveStatus status;
            curl_socket_t socket;
            {
                std::lock_guard lock(io_mutex_);
                if (!curl_) break;
                status = Receive(curl_, partial, message);
                socket = ActiveSocket(curl_);
            }
            if (status == ReceiveStatus::kClosed) break;
            if (status == ReceiveStatus::kMessage) {
                Dispatch(message);
            } else if (status == ReceiveStatus::kMore) {
                continue;
            } else if (socket == CURL_SOCKET_BAD) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                WaitSocket(socket, false, kReadPollTimeoutMs);  // without io_mutex_, so senders can proceed
            }
        }
        MarkDisconnected("Connection closed");
    }

    void HeartbeatLoop(int interval_seconds) {
        std::unique_lock lock(heartbeat_mutex_);
        while (
            !heartbeat_cv_.wait_for(lock, std::chrono::seconds(interval_seconds), [&] { return stopping_.load(); })) {
            if (!connected_) break;
            try {
                SendPacket(SerializePacket(SessionPacket(emsg::kClientHeartbeat)));
            } catch (...) {
                break;  // the reader notices the broken connection
            }
        }
    }

    std::mutex connect_mutex_;  // serializes Open() and Close()

    std::mutex io_mutex_;  // guards curl_
    CURL* curl_ = nullptr;
    // Assigned by the logon; atomic because a call racing a reconnect may still read them.
    std::atomic<std::uint64_t> steamid_{kAnonymousSteamId};
    std::atomic<std::int32_t> sessionid_{0};
    std::atomic<std::uint64_t> next_job_id_{1};

    std::mutex jobs_mutex_;  // guards pending_jobs_, and connected_ transitions to false
    std::unordered_map<std::uint64_t, std::promise<Packet>> pending_jobs_;
    std::atomic<bool> connected_{false};

    std::atomic<bool> stopping_{false};
    std::thread reader_thread_;
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
};

Session::Session() : impl_(std::make_unique<Impl>()) {}

Session::~Session() = default;

void Session::Connect() { impl_->Connect(); }

Bytes Session::Request(std::uint32_t emsg, ByteSpan body) {
    Connect();
    return impl_->Call(emsg, "", body).body;
}

Bytes Session::Rpc(std::string_view method, ByteSpan body) {
    Connect();
    Packet response = impl_->Call(emsg::kServiceMethodCall, method, body);
    if (response.eresult != kEResultOk) {
        std::string message = "RPC " + std::string(method) + " failed: EResult " + std::to_string(response.eresult);
        if (!response.error_message.empty()) message += " (" + response.error_message + ")";
        Fail(message);
    }
    return std::move(response.body);
}

}  // namespace pxsteamdl::detail
