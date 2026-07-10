// main.cpp

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <dlfcn.h>
#include <memory>
#include <string>
#include <thread>
#include <iostream>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <nlohmann/json.hpp>

#include "plugin_registry.h"
#include "thread_pool.h"


using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;
namespace fs = std::filesystem;

static fs::path exe_dir() {
    return fs::canonical("/proc/self/exe").parent_path();     // .../ifc_http_server/build
}
static fs::path server_root() {
    return exe_dir().parent_path();                           // .../ifc_http_server
}
static fs::path data_root() {
    return server_root().parent_path() / "storage";           // .../storage
}
static fs::path so_dir() {
    return exe_dir();                                         // .../ifc_http_server/build
}
static fs::path dbg_path() {
    return server_root() / "logs" / "dbg.log";
}
static fs::path ifc_upload_dir() {
    return data_root() / "uploads" / "ifc";
}
static fs::path plugins_json_path() {
    return server_root() / "config" / "plugins.json";
}


using json = nlohmann::json;
/*
static const char* kSoDir   = "/home/puyan_zadeh/ifc_http_server/build";
static const char* kDbgPath = "/home/puyan_zadeh/ifc_http_server/logs/dbg.log";
static const std::string IFC_UPLOAD_DIR = "/home/puyan_zadeh/storage/uploads/ifc/";
*/
static const std::string kSoDir   = so_dir().string();
static const std::string kDbgPath = dbg_path().string();
static const std::string IFC_UPLOAD_DIR = ifc_upload_dir().string() + "/";


static void dbg(const char* msg, const std::string& val = "") {
    FILE* f = fopen(kDbgPath.c_str(), "a");
    if (!f) return;
    if (val.empty()) fprintf(f, "%s\n", msg);
    else fprintf(f, "%s %s\n", msg, val.c_str());
    fclose(f);
}

static std::string generate_unique_name(const std::string& prefix, const std::string& ext) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << prefix << "_" << std::put_time(std::localtime(&t), "%Y%m%d_%H%M%S") << ext;
    return ss.str();
}

static std::string trim_copy(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    return s;
}

static std::string get_header_value(const http::fields& h, const std::string& key) {
    auto it = h.find(key);
    if (it == h.end()) return "";
    return std::string(it->value());
}

static std::string extract_boundary(const std::string& content_type) {
    // example: multipart/form-data; boundary=----WebKitFormBoundaryabc123
    auto pos = content_type.find("boundary=");
    if (pos == std::string::npos) return "";
    std::string b = content_type.substr(pos + 9);
    // strip optional quotes and trailing params
    auto sc = b.find(';');
    if (sc != std::string::npos) b = b.substr(0, sc);
    b = trim_copy(b);
    if (!b.empty() && b.front() == '"') b.erase(0, 1);
    if (!b.empty() && b.back() == '"') b.pop_back();
    return b;
}

// Streams multipart upload body to an IFC file, stripping multipart wrapper on the fly.
// Returns full saved IFC path on success, "" on failure.
static std::string stream_multipart_ifc_to_disk(tcp::socket& socket,
                                               boost::beast::flat_buffer& buffer,
                                               http::request_parser<http::buffer_body>& parser,
                                               const std::string& boundary)
{
    try {
        fs::create_directories(IFC_UPLOAD_DIR);
        std::string out_name = generate_unique_name("upload", ".ifc");
        std::string out_path = IFC_UPLOAD_DIR + out_name;

        std::ofstream out(out_path, std::ios::binary);
        if (!out.is_open()) return "";

        const std::string boundary_marker = "\r\n--" + boundary; // boundary begins with CRLF in body
        const std::string start_marker = "\r\n\r\n";

        bool started = false;
        bool finished = false;

        // carry keeps overlap across chunks for boundary detection
        std::string carry;

        std::array<char, 64 * 1024> buf{};
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());
        parser.get().body().data = buf.data();
        parser.get().body().size = buf.size();
        parser.get().body().more = true;

        while (!parser.is_done() && !finished) {
            boost::system::error_code ec;
            http::read(socket, buffer, parser, ec);
            if (ec && ec != http::error::need_buffer) {
                out.close();
                return "";
            }

            std::size_t bytes = buf.size() - parser.get().body().size;
            if (bytes == 0) {
                parser.get().body().data = buf.data();
                parser.get().body().size = buf.size();
                parser.get().body().more = true;
                continue;
            }

            std::string chunk(buf.data(), bytes);

            // reset buffer for next read
            parser.get().body().data = buf.data();
            parser.get().body().size = buf.size();
            parser.get().body().more = true;

            // Prepend carry so we can search across boundaries
            std::string data = carry + chunk;
            carry.clear();

            if (!started) {
                // Find the end of multipart headers for the first part
                auto p = data.find(start_marker);
                if (p == std::string::npos) {
                    // keep tail to detect start marker across chunks
                    if (data.size() > 8) carry = data.substr(data.size() - 8);
                    else carry = data;
                    continue;
                }

                started = true;
                data.erase(0, p + start_marker.size());
            }

            // If started, look for boundary that indicates end of file content
            auto bpos = data.find(boundary_marker);
            if (bpos != std::string::npos) {
                // write only up to boundary marker (exclude CRLF before boundary)
                out.write(data.data(), static_cast<std::streamsize>(bpos));
                finished = true;
                break;
            }

            // No boundary found: write all but keep a tail for boundary detection
            // Keep tail length = boundary_marker length to catch boundary across chunks
            std::size_t keep = boundary_marker.size();
            if (data.size() <= keep) {
                carry = data;
                continue;
            }

            std::size_t write_len = data.size() - keep;
            out.write(data.data(), static_cast<std::streamsize>(write_len));
            carry = data.substr(write_len);
        }

        out.close();

        if (!started || !finished) {
            // incomplete parse; delete partial file
            std::error_code rm_ec;
            fs::remove(out_path, rm_ec);
            return "";
        }

        return out_path;
    } catch (...) {
        return "";
    }
}

void handle_request(tcp::socket socket) {
    try {
        boost::beast::flat_buffer buffer;

        auto add_cors = [&](auto& res) {
            res.set(http::field::access_control_allow_origin, "*");
            res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
            res.set(http::field::access_control_allow_headers, "Content-Type, Authorization");
        };

        // Use a parser so we can stream large uploads.
        http::request_parser<http::buffer_body> parser;
        parser.body_limit((std::numeric_limits<std::uint64_t>::max)());

        // Read headers first
        http::read_header(socket, buffer, parser);

        const auto method = parser.get().method();
        const auto version = parser.get().version();
        const std::string raw_target = std::string(parser.get().target());

        if (method == http::verb::options) {
            http::response<http::empty_body> res{http::status::ok, version};
            add_cors(res);
            res.prepare_payload();
            http::write(socket, res);
            socket.shutdown(tcp::socket::shutdown_send);
            return;
        }

        if (raw_target == "/health") {
            http::response<http::string_body> res{http::status::ok, version};
            add_cors(res);
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"status":"ok"})";
            res.prepare_payload();
            http::write(socket, res);
            socket.shutdown(tcp::socket::shutdown_send);
            return;
        }

        // Determine plugin target (strip leading "/api/" and "/")
        std::string target = raw_target;
        if (target.rfind("/api/", 0) == 0) target.erase(0, 5);
        if (target.rfind('/', 0) == 0) target.erase(0, 1);
        if (target.empty()) {
            http::response<http::string_body> res{http::status::not_found, version};
            add_cors(res);
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":"route not found"})";
            res.prepare_payload();
            http::write(socket, res);
            socket.shutdown(tcp::socket::shutdown_send);
            return;
        }

        std::string handler_name = "handle_ifc_" + target;
        std::string so_path = std::string(kSoDir) + "/libifc_" + target + "_plugin.so";

        // TODO(infra): a long-running server process eventually stops picking up
        // rebuilt plugin .so's -- /proc/<pid>/maps shows a stale mapping to a
        // deleted inode, implying a leaked dlopen handle somewhere on this path
        // (possibly an exception between dlopen and dlclose below skipping the
        // close). Restarting the process is the current workaround. Deferred
        // until after Step 2-6 semantic-engine validation is stable.
        void* handle = dlopen(so_path.c_str(), RTLD_LAZY);
        if (!handle) {
            http::response<http::string_body> res{http::status::not_found, version};
            add_cors(res);
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":"plugin not found"})";
            res.prepare_payload();
            http::write(socket, res);
            socket.shutdown(tcp::socket::shutdown_send);
            return;
        }

        using func_t = const char* (*)(const std::string&);
        auto func = (func_t)dlsym(handle, handler_name.c_str());
        if (!func) {
            dlclose(handle);
            http::response<http::string_body> res{http::status::not_found, version};
            add_cors(res);
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":"handler not found"})";
            res.prepare_payload();
            http::write(socket, res);
            socket.shutdown(tcp::socket::shutdown_send);
            return;
        }

        // If this is a multipart upload (ConvertPanel), stream IFC to disk and pass JSON path to plugin.
        std::string content_type = get_header_value(parser.get().base(), "Content-Type");
        bool is_multipart = (content_type.find("multipart/form-data") != std::string::npos);

        std::string plugin_input;

        if (method == http::verb::post && is_multipart) {
            std::string boundary = extract_boundary(content_type);
            if (boundary.empty()) {
                dlclose(handle);
                http::response<http::string_body> res{http::status::bad_request, version};
                add_cors(res);
                res.set(http::field::content_type, "application/json");
                res.body() = R"({"error":"missing multipart boundary"})";
                res.prepare_payload();
                http::write(socket, res);
                socket.shutdown(tcp::socket::shutdown_send);
                return;
            }

            std::string saved_ifc = stream_multipart_ifc_to_disk(socket, buffer, parser, boundary);
            if (saved_ifc.empty()) {
                dlclose(handle);
                http::response<http::string_body> res{http::status::bad_request, version};
                add_cors(res);
                res.set(http::field::content_type, "application/json");
                res.body() = R"({"error":"upload stream failed"})";
                res.prepare_payload();
                http::write(socket, res);
                socket.shutdown(tcp::socket::shutdown_send);
                return;
            }

            plugin_input = json({{"file", saved_ifc}}).dump();
        } else {
            // For non-multipart requests, read the remaining body into a string (small JSON payloads)
            std::string body;
            std::array<char, 64 * 1024> buf{};
            parser.get().body().data = buf.data();
            parser.get().body().size = buf.size();
            parser.get().body().more = true;

            while (!parser.is_done()) {
                boost::system::error_code ec;
                http::read(socket, buffer, parser, ec);
                if (ec && ec != http::error::need_buffer) break;

                std::size_t bytes = buf.size() - parser.get().body().size;
                if (bytes > 0) body.append(buf.data(), bytes);

                parser.get().body().data = buf.data();
                parser.get().body().size = buf.size();
                parser.get().body().more = true;
            }

            plugin_input = body;
        }

        const char* output = func(plugin_input);

        http::response<http::string_body> res{http::status::ok, version};
        add_cors(res);
        res.set(http::field::content_type, "application/json");
        res.body() = output ? output : R"({"error":"empty response"})";
        res.prepare_payload();
        http::write(socket, res);

        dlclose(handle);
        socket.shutdown(tcp::socket::shutdown_send);
    }
    catch (...) {
        dbg("ERROR: handle_request");
    }
}

int main() {
    try {
        //PluginRegistry::instance().load_from_config("/home/puyan_zadeh/ifc_http_server/config/plugins.json");
        PluginRegistry::instance().load_from_config(plugins_json_path().string());


        boost::asio::io_context ioc{1};
        ThreadPool pool(std::thread::hardware_concurrency());

        tcp::acceptor acceptor{ioc, {tcp::v4(), 8080}};
        for (;;) {
            tcp::socket socket{ioc};
            acceptor.accept(socket);
            auto sp = std::make_shared<tcp::socket>(std::move(socket));
            pool.enqueue([sp]() { handle_request(std::move(*sp)); });
        }
    }
    catch (...) {
        return 1;
    }
}
