#include "McpServer.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <future>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include "nlohmann/json.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r_version.h"
#include "GUI_App.hpp"
#include "Plater.hpp"

namespace Slic3r {
namespace GUI {

using json    = nlohmann::json;
namespace http = boost::beast::http;
using tcp     = boost::asio::ip::tcp;

static const char *MCP_PROTOCOL_VERSION = "2025-06-18";

// Runs f on the UI thread and waits for the result. Gives up when the server stops,
// because the UI thread may be the one stopping it and would never run f.
static json run_on_ui_thread(const std::atomic_bool &running, std::function<json()> f)
{
    auto promise = std::make_shared<std::promise<json>>();
    auto future  = promise->get_future();
    wxGetApp().CallAfter([promise, f] {
        try {
            promise->set_value(f());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (!running)
            throw std::runtime_error("BambuStudio is shutting down");
    return future.get();
}

static json to_json(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }

static json get_scene(const json &)
{
    json objects = json::array();
    for (const ModelObject *obj : wxGetApp().plater()->model().objects) {
        json instances = json::array();
        for (size_t i = 0; i < obj->instances.size(); ++i) {
            BoundingBoxf3 bb = obj->instance_bounding_box(i);
            instances.push_back({{"min", to_json(bb.min)}, {"max", to_json(bb.max)}, {"size", to_json(bb.size())}});
        }
        objects.push_back({{"name", obj->name}, {"instances", instances}});
    }
    return {{"units", "mm"}, {"objects", objects}};
}

struct McpTool
{
    const char                     *name;
    const char                     *description;
    json                            input_schema;
    std::function<json(const json &)> run;
};

static const std::vector<McpTool> &tools()
{
    static const std::vector<McpTool> tools = {
        {"get_scene", "List the objects on the build plate, with the bounding box (min, max, size in mm) of each instance.",
         {{"type", "object"}, {"properties", json::object()}}, get_scene},
    };
    return tools;
}

static json rpc_result(const json &id, const json &result) { return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; }

static json rpc_error(const json &id, int code, const std::string &message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

static json tool_result(const std::string &text, bool is_error)
{
    json content = json::array();
    content.push_back({{"type", "text"}, {"text", text}});
    return {{"content", content}, {"isError", is_error}};
}

// Returns the JSON-RPC response, or null for a notification (which gets no response).
static json handle_rpc(const json &req, const std::atomic_bool &running)
{
    if (!req.is_object() || !req.contains("method") || !req["method"].is_string())
        return rpc_error(nullptr, -32600, "Invalid request");
    if (!req.contains("id"))
        return nullptr;

    const json        &id     = req["id"];
    const std::string  method = req["method"];
    const json         params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();

    if (method == "initialize")
        return rpc_result(id, {{"protocolVersion", params.value("protocolVersion", MCP_PROTOCOL_VERSION)},
                               {"capabilities", {{"tools", json::object()}}},
                               {"serverInfo", {{"name", "bambustudio"}, {"version", SLIC3R_VERSION}}}});
    if (method == "ping")
        return rpc_result(id, json::object());
    if (method == "tools/list") {
        json list = json::array();
        for (const McpTool &tool : tools())
            list.push_back({{"name", tool.name}, {"description", tool.description}, {"inputSchema", tool.input_schema}});
        return rpc_result(id, {{"tools", list}});
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        auto tool = std::find_if(tools().begin(), tools().end(), [&name](const McpTool &t) { return name == t.name; });
        if (tool == tools().end())
            return rpc_error(id, -32602, "Unknown tool: " + name);
        const json args = params.contains("arguments") ? params["arguments"] : json::object();
        try {
            return rpc_result(id, tool_result(run_on_ui_thread(running, [tool, args] { return tool->run(args); }).dump(2), false));
        } catch (const std::exception &e) {
            return rpc_result(id, tool_result(e.what(), true));
        }
    }
    return rpc_error(id, -32601, "Method not found: " + method);
}

// Browsers send Origin; only accept local pages, so a remote site cannot drive the app (DNS rebinding).
static bool is_local_origin(const std::string &origin)
{
    for (const char *host : {"http://localhost", "http://127.0.0.1", "https://localhost", "https://127.0.0.1"}) {
        const size_t n = strlen(host);
        if (boost::starts_with(origin, host) && (origin.size() == n || origin[n] == ':'))
            return true;
    }
    return false;
}

struct McpSession : std::enable_shared_from_this<McpSession>
{
    McpSession(tcp::socket s, const std::atomic_bool &running) : socket(std::move(s)), running(running) {}

    void run()
    {
        auto self = shared_from_this();
        http::async_read(socket, buffer, req, [self](boost::beast::error_code ec, std::size_t) {
            if (!ec)
                self->respond();
        });
    }

    void respond()
    {
        auto res = std::make_shared<http::response<http::string_body>>(http::status::ok, req.version());
        res->keep_alive(false);

        const std::string origin(req[http::field::origin]);
        if (!origin.empty() && !is_local_origin(origin))
            res->result(http::status::forbidden);
        else if (req.target() != "/mcp")
            res->result(http::status::not_found);
        else if (req.method() != http::verb::post) {
            // GET would open a server-to-client event stream, which this server does not offer.
            res->result(http::status::method_not_allowed);
            res->set(http::field::allow, "POST");
        } else {
            json reply;
            try {
                const json body = json::parse(req.body(), nullptr, false);
                reply = body.is_discarded() ? rpc_error(nullptr, -32700, "Parse error") : handle_rpc(body, running);
            } catch (const std::exception &e) {
                reply = rpc_error(nullptr, -32603, e.what());
            }
            if (reply.is_null())
                res->result(http::status::accepted);
            else {
                res->set(http::field::content_type, "application/json");
                res->body() = reply.dump();
            }
        }
        res->prepare_payload();

        auto self = shared_from_this();
        http::async_write(socket, *res, [self, res](boost::beast::error_code ec, std::size_t) {
            self->socket.shutdown(tcp::socket::shutdown_send, ec);
        });
    }

    tcp::socket                       socket;
    boost::beast::flat_buffer         buffer;
    http::request<http::string_body>  req;
    const std::atomic_bool           &running;
};

void McpServer::start()
{
    if (m_running.exchange(true))
        return;

    tcp::endpoint             ep{boost::asio::ip::make_address("127.0.0.1"), MCP_PORT};
    boost::system::error_code ec;
    m_acceptor.open(ep.protocol(), ec);
    if (!ec) m_acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    if (!ec) m_acceptor.bind(ep, ec);
    if (!ec) m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "MCP server: cannot listen on port " << MCP_PORT << ": " << ec.message();
        m_acceptor.close(ec);
        m_running = false;
        return;
    }

    do_accept();
    m_worker = boost::thread([this] { m_io.run(); });
    BOOST_LOG_TRIVIAL(info) << "MCP server listening on http://127.0.0.1:" << MCP_PORT << "/mcp";
}

void McpServer::stop()
{
    if (!m_running.exchange(false))
        return;
    m_io.stop();
    if (m_worker.joinable())
        m_worker.join();
    boost::system::error_code ec;
    m_acceptor.close(ec);
}

void McpServer::do_accept()
{
    m_acceptor.async_accept([this](boost::system::error_code ec, tcp::socket s) {
        if (!ec && m_running)
            std::make_shared<McpSession>(std::move(s), m_running)->run();
        if (m_running)
            do_accept();
    });
}

} // namespace GUI
} // namespace Slic3r
