// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace pxsteamdl::detail {

namespace {

// Waits up to timeout_ms for the socket to become readable (or writable); returns false on timeout or error.
bool wait_socket(curl_socket_t socket, bool write, int timeout_ms) {
#ifdef _WIN32
    WSAPOLLFD entry{socket, static_cast<SHORT>(write ? POLLOUT : POLLIN), 0};
    return WSAPoll(&entry, 1, timeout_ms) > 0;
#else
    pollfd entry{socket, static_cast<short>(write ? POLLOUT : POLLIN), 0};
    return poll(&entry, 1, timeout_ms) > 0;
#endif
}

constexpr std::uint64_t no_job = ~0ULL;

Bytes decompress_gzip(std::span<const std::uint8_t> data, std::size_t uncompressed_hint) {
    // The hint comes from the network: cap the initial allocation, the buffer grows as needed.
    constexpr std::size_t max_initial = 16 << 20;
    Bytes out(std::clamp<std::size_t>(uncompressed_hint ? uncompressed_hint : data.size() * 4, 1024, max_initial));
    z_stream strm{};
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK)
        throw std::runtime_error("inflateInit2 failed");

    strm.next_in = const_cast<Bytef*>(data.data());
    strm.avail_in = static_cast<uInt>(data.size());

    while (true) {
        strm.next_out = reinterpret_cast<Bytef*>(out.data() + strm.total_out);
        strm.avail_out = static_cast<uInt>(out.size() - strm.total_out);
        int rc = inflate(&strm, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK) {
            inflateEnd(&strm);
            throw std::runtime_error("gzip decompression failed (" + std::to_string(rc) + ")");
        }
        if (strm.avail_out == 0) out.resize(out.size() * 2);
    }
    out.resize(strm.total_out);
    inflateEnd(&strm);
    return out;
}

struct Packet {
    std::uint32_t emsg = 0;
    std::uint64_t steamid = 0;
    std::int32_t sessionid = 0;
    std::uint64_t jobid_source = no_job;
    std::uint64_t jobid_target = no_job;
    std::int32_t eresult = 0;
    std::string target_job_name;
    std::string error_message;
    Bytes body;
};

Packet parse_proto_packet(std::span<const std::uint8_t> data) {
    if (data.size() < 8) throw std::runtime_error("packet too short for header");
    std::uint32_t raw_emsg = 0, hdr_len = 0;
    std::memcpy(&raw_emsg, data.data(), 4);
    std::memcpy(&hdr_len, data.data() + 4, 4);
    if (data.size() < 8 + hdr_len) throw std::runtime_error("packet header truncated");

    Packet pkt;
    pkt.emsg = raw_emsg & ~0x80000000u;
    Reader r(data.subspan(8, hdr_len));
    Field f;
    while (r.next(f)) {
        switch (f.number) {
        case 1: if (f.wire == 1) pkt.steamid = f.integer; break;
        case 2: if (f.wire == 0) pkt.sessionid = static_cast<std::int32_t>(f.integer); break;
        case 10: if (f.wire == 1) pkt.jobid_source = f.integer; break;
        case 11: if (f.wire == 1) pkt.jobid_target = f.integer; break;
        case 12: if (f.wire == 2) pkt.target_job_name.assign(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()); break;
        case 13: if (f.wire == 0) pkt.eresult = static_cast<std::int32_t>(f.integer); break;
        case 14: if (f.wire == 2) pkt.error_message.assign(reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()); break;
        default: break;
        }
    }
    pkt.body.assign(data.begin() + 8 + hdr_len, data.end());
    return pkt;
}

Bytes serialize_packet(std::uint32_t emsg, std::uint64_t steamid, std::int32_t sessionid,
                       std::uint64_t jobid_src, std::uint64_t jobid_tgt,
                       std::string_view target_job_name,
                       std::span<const std::uint8_t> body) {
    Bytes header;
    auto append = [&](Bytes b) { header.insert(header.end(), b.begin(), b.end()); };
    if (steamid) append(encode_fixed64(1, steamid));
    if (sessionid) append(encode_uint(2, static_cast<std::uint32_t>(sessionid)));
    if (jobid_src != no_job) append(encode_fixed64(10, jobid_src));
    if (jobid_tgt != no_job) append(encode_fixed64(11, jobid_tgt));
    if (!target_job_name.empty()) append(encode_string(12, target_job_name));

    Bytes out(8);
    std::uint32_t raw_emsg = emsg | 0x80000000u;
    std::uint32_t hdr_len = static_cast<std::uint32_t>(header.size());
    std::memcpy(&out[0], &raw_emsg, 4);
    std::memcpy(&out[4], &hdr_len, 4);
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Parses a CM message, expanding a Multi into the packets it carries.
std::vector<Packet> parse_message(std::span<const std::uint8_t> message) {
    Packet pkt = parse_proto_packet(message);
    std::vector<Packet> out;
    if (pkt.emsg != emsg::multi) {
        out.push_back(std::move(pkt));
        return out;
    }
    Reader r(pkt.body);
    Field f;
    std::uint64_t unzipped_size = 0;
    std::span<const std::uint8_t> payload;
    while (r.next(f)) {
        if (f.number == 1 && f.wire == 0) unzipped_size = f.integer;
        else if (f.number == 2 && f.wire == 2) payload = f.bytes;
    }
    Bytes decomp;
    std::span<const std::uint8_t> data;
    if (unzipped_size > 0) {
        decomp = decompress_gzip(payload, unzipped_size);
        data = decomp;
    } else {
        data = payload;
    }

    std::size_t offset = 0;
    while (offset + 4 <= data.size()) {
        std::uint32_t sub_len = 0;
        std::memcpy(&sub_len, data.data() + offset, 4);
        offset += 4;
        if (offset + sub_len > data.size()) break;
        out.push_back(parse_proto_packet(data.subspan(offset, sub_len)));
        offset += sub_len;
    }
    return out;
}

// again: nothing buffered, wait for the socket; more: read again right away.
enum class Receive { message, more, again, closed };

// One non-blocking read from a CONNECT_ONLY WebSocket. Data frames are reassembled in partial; a completed message
// is moved into message. Control frames (curl answers pings itself) are skipped.
Receive receive(CURL* curl, Bytes& partial, Bytes& message) {
    std::uint8_t buffer[65536];
    std::size_t read = 0;
    const curl_ws_frame* meta = nullptr;
    CURLcode rc = curl_ws_recv(curl, buffer, sizeof buffer, &read, &meta);
    if (rc == CURLE_AGAIN) return Receive::again;
    if (rc != CURLE_OK || !meta || (meta->flags & CURLWS_CLOSE)) return Receive::closed;
    if (!(meta->flags & (CURLWS_BINARY | CURLWS_TEXT))) return Receive::more;
    partial.insert(partial.end(), buffer, buffer + read);
    if (meta->bytesleft != 0 || (meta->flags & CURLWS_CONT)) return Receive::more;
    message = std::move(partial);
    partial.clear();
    return Receive::message;
}

curl_socket_t active_socket(CURL* curl) {
    curl_socket_t socket = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
    return socket;
}

std::vector<std::string> cm_endpoints() {
    HttpResponse response = http_request("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0");
    if (response.status != 200)
        throw std::runtime_error("GetCMListForConnect: HTTP " + std::to_string(response.status));
    auto root = nlohmann::json::parse(response.body.begin(), response.body.end());
    const auto& serverlist = root.at("response").at("serverlist");
    if (!serverlist.is_array()) throw std::runtime_error("GetCMListForConnect: invalid serverlist");

    std::vector<std::string> endpoints;
    for (const auto& server : serverlist) {
        if (server.value("type", "") != "websockets" || !server.contains("endpoint") || !server["endpoint"].is_string())
            continue;
        auto endpoint = server["endpoint"].get<std::string>();
        if (!endpoint.empty()) endpoints.push_back(std::move(endpoint));
    }
    if (endpoints.empty()) throw std::runtime_error("No websocket CM endpoints found");
    return endpoints;
}

} // namespace

struct Session::Impl {
    std::mutex connect_mutex; // serializes open() and close()

    std::mutex io_mutex; // guards curl
    CURL* curl = nullptr;
    // Assigned by the logon; atomic because a call racing a reconnect may still read them.
    std::atomic<std::uint64_t> steamid{anonymous_steamid};
    std::atomic<std::int32_t> sessionid{0};
    std::atomic<std::uint64_t> next_job_id{1};

    std::mutex jobs_mutex; // guards pending_jobs, and connected transitions to false
    std::unordered_map<std::uint64_t, std::promise<Packet>> pending_jobs;
    std::atomic<bool> connected{false};

    std::atomic<bool> stopping{false};
    std::thread reader_thread;
    std::thread heartbeat_thread;
    std::mutex heartbeat_mutex;
    std::condition_variable heartbeat_cv;

    static constexpr std::uint64_t anonymous_steamid = 0x01A0000000000000ULL;

    ~Impl() {
        std::lock_guard lock(connect_mutex);
        close();
    }

    // Logs off (if still connected), stops the threads and releases the socket. Requires connect_mutex.
    void close() {
        bool was_connected = mark_disconnected("Session disconnected");
        stop_threads();
        if (was_connected) {
            try {
                send_packet(serialize_packet(emsg::client_log_off, steamid, sessionid, no_job, no_job, "", {}));
            } catch (...) {}
        }
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        if (reader_thread.joinable()) reader_thread.join();

        std::lock_guard lock(io_mutex);
        if (curl) curl_easy_cleanup(curl);
        curl = nullptr;
    }

    void stop_threads() {
        {
            std::lock_guard lock(heartbeat_mutex); // no lost wakeup between the heartbeat's check and wait
            stopping = true;
        }
        heartbeat_cv.notify_all();
    }

    // Stops accepting calls and fails the ones in flight; returns whether the session was connected.
    bool mark_disconnected(const char* reason) {
        std::lock_guard lock(jobs_mutex);
        bool was_connected = connected.exchange(false);
        for (auto& [id, promise] : pending_jobs)
            promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
        pending_jobs.clear();
        return was_connected;
    }

    // Tries the CM endpoints in turn. Requires connect_mutex and a closed session.
    void open() {
        std::vector<std::string> endpoints = cm_endpoints();
        constexpr std::size_t max_attempts = 5;
        std::string last_error = "no endpoint reachable";
        for (std::size_t i = 0; i < std::min(endpoints.size(), max_attempts); ++i) {
            try {
                if (logon("wss://" + endpoints[i] + "/cmsocket/")) return;
                last_error = "no logon response";
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            std::lock_guard lock(io_mutex);
            if (curl) curl_easy_cleanup(curl);
            curl = nullptr;
        }
        throw std::runtime_error("Failed to connect to Steam CM: " + last_error);
    }

    // Opens the socket and logs on anonymously; on success starts the reader and heartbeat threads.
    // Returns false on a connection failure or timeout, throws on a rejected logon.
    bool logon(const std::string& url) {
        CURL* handle = curl_easy_init();
        if (!handle) throw std::runtime_error("curl_easy_init failed");
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        set_ca_bundle(handle);
        curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
        {
            std::lock_guard lock(io_mutex);
            curl = handle; // owned (and cleaned up on failure) by open()
        }
        if (curl_easy_perform(handle) != CURLE_OK) return false;

        Bytes logon_body = concat({
            encode_uint(1, 65581),        // protocol version
            encode_uint(3, 0),            // cell id
            encode_string(6, "english"),  // language
            encode_uint(7, 4294967112u),  // client_os_type: Linux 6.x (-184)
        });
        send_packet(serialize_packet(emsg::client_logon, anonymous_steamid, 0, no_job, no_job, "", logon_body));

        // No other thread uses the handle yet, so the response is read synchronously.
        Bytes partial, message;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            Receive result = receive(handle, partial, message);
            if (result == Receive::closed) return false;
            if (result == Receive::more) continue;
            if (result == Receive::again) {
                wait_socket(active_socket(handle), false, 50);
                continue;
            }
            for (const auto& packet : parse_message(message)) {
                if (packet.emsg != emsg::client_logon_response) continue;
                int eresult = packet.eresult;
                int heartbeat_seconds = 9;
                Reader reader(packet.body);
                Field field;
                while (reader.next(field)) {
                    if (field.number == 1 && field.wire == 0) eresult = static_cast<int>(field.integer);
                    else if (field.number == 3 && field.wire == 0) heartbeat_seconds = static_cast<int>(field.integer);
                }
                if (eresult != 1) throw std::runtime_error("Logon failed with EResult " + std::to_string(eresult));
                steamid = packet.steamid;
                sessionid = packet.sessionid;
                stopping = false;
                connected = true;
                reader_thread = std::thread(&Impl::reader_loop, this);
                heartbeat_thread = std::thread(&Impl::heartbeat_loop, this, std::clamp(heartbeat_seconds, 1, 60));
                return true;
            }
        }
        return false;
    }

    void send_packet(std::span<const std::uint8_t> data) {
        std::lock_guard lock(io_mutex);
        if (!curl) throw std::runtime_error("Session not connected");
        curl_socket_t socket = active_socket(curl);
        for (std::size_t offset = 0; offset < data.size();) {
            std::size_t sent = 0;
            CURLcode rc = curl_ws_send(curl, data.data() + offset, data.size() - offset, &sent, 0, CURLWS_BINARY);
            if (rc == CURLE_AGAIN) {
                if (socket == CURL_SOCKET_BAD) throw std::runtime_error("curl socket unavailable for send");
                if (!wait_socket(socket, true, 5000)) throw std::runtime_error("curl_ws_send poll timeout");
                continue;
            }
            if (rc != CURLE_OK) throw std::runtime_error(std::string("curl_ws_send failed: ") + curl_easy_strerror(rc));
            offset += sent;
        }
    }

    void dispatch(std::span<const std::uint8_t> message) {
        std::vector<Packet> packets;
        try {
            packets = parse_message(message);
        } catch (const std::exception&) {
            return; // malformed message: nothing to route
        }
        for (auto& packet : packets) {
            if (packet.emsg == emsg::client_logged_off) stop_threads();
            if (packet.jobid_target == no_job) continue;
            std::promise<Packet> promise;
            {
                std::lock_guard lock(jobs_mutex);
                auto it = pending_jobs.find(packet.jobid_target);
                if (it == pending_jobs.end()) continue;
                promise = std::move(it->second);
                pending_jobs.erase(it);
            }
            promise.set_value(std::move(packet));
        }
    }

    void reader_loop() {
        Bytes partial, message;
        while (!stopping) {
            Receive result;
            curl_socket_t socket;
            {
                std::lock_guard lock(io_mutex);
                if (!curl) break;
                result = receive(curl, partial, message);
                socket = active_socket(curl);
            }
            if (result == Receive::closed) break;
            if (result == Receive::message) dispatch(message);
            else if (result == Receive::more) continue;
            else if (socket == CURL_SOCKET_BAD) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            else wait_socket(socket, false, 50); // without holding io_mutex, so senders can proceed
        }
        mark_disconnected("Connection closed");
    }

    void heartbeat_loop(int interval_seconds) {
        std::unique_lock lock(heartbeat_mutex);
        while (!heartbeat_cv.wait_for(lock, std::chrono::seconds(interval_seconds), [&] { return stopping.load(); })) {
            if (!connected) break;
            try {
                send_packet(serialize_packet(emsg::client_heartbeat, steamid, sessionid, no_job, no_job, "", {}));
            } catch (...) {
                break; // the reader notices the broken connection
            }
        }
    }

    Packet call(std::uint32_t type, std::string_view job_name, std::span<const std::uint8_t> body) {
        std::uint64_t jobid = next_job_id++;
        std::future<Packet> future;
        {
            // Checked under jobs_mutex so the job cannot be registered after the reader failed the pending ones.
            std::lock_guard lock(jobs_mutex);
            if (!connected) throw std::runtime_error("Session not connected");
            future = pending_jobs[jobid].get_future();
        }
        auto forget = [&] {
            std::lock_guard lock(jobs_mutex);
            pending_jobs.erase(jobid);
        };
        try {
            send_packet(serialize_packet(type, steamid, sessionid, jobid, no_job, job_name, body));
        } catch (...) {
            forget();
            throw;
        }
        if (future.wait_for(std::chrono::seconds(30)) == std::future_status::timeout) {
            forget();
            throw std::runtime_error("Steam CM request timed out (EMsg " + std::to_string(type) + " " +
                                     std::string(job_name) + ")");
        }
        return future.get();
    }
};

Session::Session() : impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

void Session::connect() {
    std::lock_guard lock(impl_->connect_mutex);
    if (impl_->connected) return;
    impl_->close(); // joins the threads of a dropped connection
    impl_->open();
}

Bytes Session::request(std::uint32_t type, std::span<const std::uint8_t> body) {
    connect();
    return impl_->call(type, "", body).body;
}

Bytes Session::rpc(std::string_view method, std::span<const std::uint8_t> body) {
    connect();
    Packet response = impl_->call(emsg::service_method_call, method, body);
    if (response.eresult != 1) {
        std::string message = "RPC " + std::string(method) + " failed: EResult " + std::to_string(response.eresult);
        if (!response.error_message.empty()) message += " (" + response.error_message + ")";
        throw std::runtime_error(message);
    }
    return std::move(response.body);
}

} // namespace pxsteamdl::detail
