// SPDX-License-Identifier: LGPL-3.0-or-later
#include "internal.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <zlib.h>

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

namespace pxsteamdl::detail {

namespace {

Bytes decompress_gzip(std::span<const std::uint8_t> data, std::size_t uncompressed_hint) {
    Bytes out(uncompressed_hint ? uncompressed_hint : data.size() * 4);
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
    std::uint64_t jobid_source = ~0ULL;
    std::uint64_t jobid_target = ~0ULL;
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
    if (jobid_src != ~0ULL) append(encode_fixed64(10, jobid_src));
    if (jobid_tgt != ~0ULL) append(encode_fixed64(11, jobid_tgt));
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

void unpack_packet(const Packet& pkt, std::vector<Packet>& out) {
    if (pkt.emsg != 1) { // not Multi
        out.push_back(pkt);
        return;
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
}

} // namespace

struct Session::Impl {
    std::mutex io_mutex;
    CURL* curl = nullptr;
    std::uint64_t steamid = 0x01A0000000000000ULL;
    std::int32_t sessionid = 0;
    std::atomic<std::uint64_t> next_job_id{1};

    std::mutex jobs_mutex;
    std::unordered_map<std::uint64_t, std::promise<Packet>> pending_jobs;

    std::atomic<bool> connected{false};
    std::atomic<bool> stopping{false};
    std::thread reader_thread;
    std::thread heartbeat_thread;
    std::condition_variable heartbeat_cv;
    std::mutex heartbeat_mutex;

    ~Impl() {
        disconnect();
    }

    void disconnect() {
        bool was_connected = connected.exchange(false);
        stopping.store(true);
        heartbeat_cv.notify_all();

        if (was_connected) {
            // Best effort ClientLogOff (706)
            try {
                auto pkt = serialize_packet(706, steamid, sessionid, ~0ULL, ~0ULL, "", {});
                send_packet(pkt);
            } catch (...) {}
        }

        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        if (reader_thread.joinable()) reader_thread.join();

        {
            std::lock_guard<std::mutex> lk(jobs_mutex);
            for (auto& [id, prom] : pending_jobs) {
                try {
                    prom.set_exception(std::make_exception_ptr(std::runtime_error("Session disconnected")));
                } catch (...) {}
            }
            pending_jobs.clear();
        }

        {
            std::lock_guard<std::mutex> lk(io_mutex);
            if (curl) {
                curl_easy_cleanup(curl);
                curl = nullptr;
            }
        }
    }

    void send_packet(std::span<const std::uint8_t> data) {
        std::lock_guard<std::mutex> lk(io_mutex);
        if (!curl) throw std::runtime_error("Session not connected");

        curl_socket_t sock = CURL_SOCKET_BAD;
        curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);

        std::size_t offset = 0;
        while (offset < data.size()) {
            std::size_t sent = 0;
            CURLcode rc = curl_ws_send(curl, data.data() + offset, data.size() - offset, &sent, 0, CURLWS_BINARY);
            if (rc == CURLE_AGAIN) {
                if (sock == CURL_SOCKET_BAD) throw std::runtime_error("curl socket unavailable for send");
                struct pollfd pfd;
                pfd.fd = sock;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                int pr = poll(&pfd, 1, 5000);
                if (pr <= 0) throw std::runtime_error("curl_ws_send poll timeout");
                continue;
            }
            if (rc != CURLE_OK) throw std::runtime_error(std::string("curl_ws_send failed: ") + curl_easy_strerror(rc));
            offset += sent;
        }
    }

    void dispatch_frame(const Bytes& frame) {
        try {
            Packet raw = parse_proto_packet(frame);
            std::vector<Packet> packets;
            unpack_packet(raw, packets);
            for (auto& p : packets) {
                if (p.emsg == 757) { // ClientLoggedOff
                    connected.store(false);
                    stopping.store(true);
                }
                if (p.jobid_target != ~0ULL) {
                    std::promise<Packet> prom;
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> lk(jobs_mutex);
                        auto it = pending_jobs.find(p.jobid_target);
                        if (it != pending_jobs.end()) {
                            prom = std::move(it->second);
                            pending_jobs.erase(it);
                            found = true;
                        }
                    }
                    if (found) prom.set_value(std::move(p));
                }
            }
        } catch (...) {}
    }

    void reader_loop() {
        Bytes frame_accumulator;
        while (!stopping.load()) {
            std::uint8_t buf[65536];
            std::size_t nread = 0;
            const struct curl_ws_frame* meta = nullptr;
            CURLcode rc;
            curl_socket_t sock = CURL_SOCKET_BAD;

            {
                std::lock_guard<std::mutex> lk(io_mutex);
                if (!curl) break;
                curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock);
                rc = curl_ws_recv(curl, buf, sizeof(buf), &nread, &meta);
            }

            if (rc == CURLE_AGAIN) {
                if (sock == CURL_SOCKET_BAD) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                struct pollfd pfd;
                pfd.fd = sock;
                pfd.events = POLLIN;
                pfd.revents = 0;
                poll(&pfd, 1, 50); // 50ms poll without holding io_mutex
                continue;
            }

            if (rc != CURLE_OK) break;
            if (meta && (meta->flags & CURLWS_CLOSE)) break;

            frame_accumulator.insert(frame_accumulator.end(), buf, buf + nread);
            if (meta && meta->bytesleft == 0) {
                Bytes frame = std::move(frame_accumulator);
                frame_accumulator.clear();
                dispatch_frame(frame);
            }
        }

        // Connection closed or error
        connected.store(false);
        std::lock_guard<std::mutex> lk(jobs_mutex);
        for (auto& [id, prom] : pending_jobs) {
            try {
                prom.set_exception(std::make_exception_ptr(std::runtime_error("Connection closed")));
            } catch (...) {}
        }
        pending_jobs.clear();
    }

    void heartbeat_loop(int interval_sec) {
        while (!stopping.load()) {
            std::unique_lock<std::mutex> lk(heartbeat_mutex);
            if (heartbeat_cv.wait_for(lk, std::chrono::seconds(interval_sec), [&] { return stopping.load(); }))
                break;

            try {
                if (connected.load()) {
                    auto pkt = serialize_packet(703, steamid, sessionid, ~0ULL, ~0ULL, "", {});
                    send_packet(pkt);
                }
            } catch (...) {
                break;
            }
        }
    }

    Packet call(std::uint32_t emsg, std::string_view job_name, std::span<const std::uint8_t> body) {
        if (!connected.load()) throw std::runtime_error("Session not connected");

        std::uint64_t jobid = next_job_id.fetch_add(1);
        std::future<Packet> fut;
        {
            std::lock_guard<std::mutex> lk(jobs_mutex);
            fut = pending_jobs[jobid].get_future();
        }

        try {
            send_packet(serialize_packet(emsg, steamid, sessionid, jobid, ~0ULL, job_name, body));
        } catch (...) {
            std::lock_guard<std::mutex> lk(jobs_mutex);
            pending_jobs.erase(jobid);
            throw;
        }

        if (fut.wait_for(std::chrono::seconds(30)) == std::future_status::timeout) {
            std::lock_guard<std::mutex> lk(jobs_mutex);
            pending_jobs.erase(jobid);
            throw std::runtime_error("Steam CM request timed out (EMsg " + std::to_string(emsg) + " " +
                                     std::string(job_name) + ")");
        }
        return fut.get();
    }
};

Session::Session() : impl_(new Impl) {}
Session::~Session() { delete impl_; }

void Session::connect() {
    if (impl_->connected.load()) throw std::runtime_error("Session already connected");

    HttpResponse dir_resp = http_request("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0");
    if (dir_resp.status != 200)
        throw std::runtime_error("GetCMListForConnect: HTTP " + std::to_string(dir_resp.status));

    auto root = nlohmann::json::parse(std::string_view(reinterpret_cast<const char*>(dir_resp.body.data()),
                                                       dir_resp.body.size()));
    const auto& serverlist = root.at("response").at("serverlist");
    if (!serverlist.is_array())
        throw std::runtime_error("GetCMListForConnect: invalid serverlist");

    std::vector<std::string> endpoints;
    for (const auto& item : serverlist) {
        if (item.value("type", "") == "websockets" && item.contains("endpoint")) {
            std::string ep = item["endpoint"].get<std::string>();
            if (!ep.empty()) endpoints.push_back(std::move(ep));
        }
    }

    if (endpoints.empty()) throw std::runtime_error("No websocket CM endpoints found");

    constexpr std::size_t max_attempts = 5;
    std::size_t attempts = std::min(endpoints.size(), max_attempts);
    std::string last_error;

    for (std::size_t i = 0; i < attempts; ++i) {
        const std::string& ep = endpoints[i];
        std::string url = "wss://" + ep + "/cmsocket/";

        CURL* c = curl_easy_init();
        if (!c) continue;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);

        if (curl_easy_perform(c) != CURLE_OK) {
            curl_easy_cleanup(c);
            continue;
        }

        impl_->curl = c;

        // Build ClientLogon (5514)
        Bytes logon_body;
        auto append = [&](Bytes b) { logon_body.insert(logon_body.end(), b.begin(), b.end()); };
        append(encode_uint(1, 65581));       // protocol version
        append(encode_uint(3, 0));           // cell id
        append(encode_string(6, "english")); // language
        append(encode_uint(7, 4294967112u)); // client_os_type: Linux 6.x (-184)
        auto logon_pkt = serialize_packet(5514, impl_->steamid, 0, ~0ULL, ~0ULL, "", logon_body);

        try {
            impl_->send_packet(logon_pkt);

            // Read logon response (751) synchronously
            int heartbeat_seconds = 9;
            bool logon_ok = false;
            int eresult = 0;

            Bytes frame_acc;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline && !logon_ok) {
                std::uint8_t buf[65536];
                std::size_t nread = 0;
                const struct curl_ws_frame* meta = nullptr;
                CURLcode rc = curl_ws_recv(c, buf, sizeof(buf), &nread, &meta);
                if (rc == CURLE_AGAIN) {
                    curl_socket_t sock;
                    curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock);
                    struct pollfd pfd;
                    pfd.fd = sock;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    poll(&pfd, 1, 50);
                    continue;
                }
                if (rc != CURLE_OK) break;
                if (meta && (meta->flags & CURLWS_CLOSE)) break;

                frame_acc.insert(frame_acc.end(), buf, buf + nread);
                if (meta && meta->bytesleft == 0) {
                    Bytes frame = std::move(frame_acc);
                    frame_acc.clear();

                    Packet raw = parse_proto_packet(frame);
                    std::vector<Packet> packets;
                    unpack_packet(raw, packets);
                    for (const auto& p : packets) {
                        if (p.emsg == 751) { // ClientLogOnResponse
                            eresult = p.eresult;
                            Reader br(p.body);
                            Field f;
                            while (br.next(f)) {
                                if (f.number == 1 && f.wire == 0) eresult = static_cast<int>(f.integer);
                                else if (f.number == 3 && f.wire == 0) heartbeat_seconds = static_cast<int>(f.integer);
                            }
                            if (eresult == 1) {
                                impl_->steamid = p.steamid;
                                impl_->sessionid = p.sessionid;
                                logon_ok = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (logon_ok) {
                impl_->connected.store(true);
                impl_->reader_thread = std::thread(&Impl::reader_loop, impl_);
                impl_->heartbeat_thread = std::thread(&Impl::heartbeat_loop, impl_, heartbeat_seconds);
                return; // successfully connected!
            }

            last_error = "Logon failed with EResult " + std::to_string(eresult);
        } catch (const std::exception& ex) {
            last_error = ex.what();
        }

        // Clean up before trying next CM
        curl_easy_cleanup(c);
        impl_->curl = nullptr;
    }

    throw std::runtime_error("Failed to connect to Steam CM: " + last_error);
}

Bytes Session::request(std::uint32_t emsg, std::span<const std::uint8_t> body) {
    return impl_->call(emsg, "", body).body;
}

Bytes Session::rpc(std::string_view method, std::span<const std::uint8_t> body) {
    Packet resp = impl_->call(151, method, body);
    if (resp.eresult != 1) {
        std::string msg = "RPC " + std::string(method) + " failed: EResult " + std::to_string(resp.eresult);
        if (!resp.error_message.empty()) msg += " (" + resp.error_message + ")";
        throw std::runtime_error(msg);
    }
    return std::move(resp.body);
}

} // namespace pxsteamdl::detail
