#include "McpServer.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/detail/base64.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include "nlohmann/json.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r_version.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GLToolbar.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Jobs/Job.hpp"

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

// Arrange and slicing finish through UI events, so poll their state on the UI thread.
static void wait_until(const std::atomic_bool &running, std::function<bool()> done, int timeout_s, const std::string &what)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (!run_on_ui_thread(running, [done] { return json(done()); }).get<bool>()) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error(what + " did not finish within " + std::to_string(timeout_s) + " s");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

static json to_json(const Vec3d &v) { return {v.x(), v.y(), v.z()}; }

static json object_json(size_t idx)
{
    const ModelObject *obj       = wxGetApp().plater()->model().objects[idx];
    json               instances = json::array();
    for (size_t i = 0; i < obj->instances.size(); ++i) {
        BoundingBoxf3 bb = obj->instance_bounding_box(i);
        instances.push_back({{"min", to_json(bb.min)}, {"max", to_json(bb.max)}, {"size", to_json(bb.size())}});
    }
    return {{"index", idx}, {"name", obj->name}, {"instances", instances}};
}

static json get_scene(const json &, const std::atomic_bool &running)
{
    return run_on_ui_thread(running, [] {
        json objects = json::array();
        for (size_t i = 0; i < wxGetApp().plater()->model().objects.size(); ++i)
            objects.push_back(object_json(i));
        return json{{"units", "mm"}, {"objects", objects}};
    });
}

// Adds the mesh as a new object on a free spot of the plate.
static json add_mesh_object(const std::atomic_bool &running, const TriangleMesh &mesh, const std::string &name)
{
    return run_on_ui_thread(running, [mesh, name] {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add Primitive");
        wxGetApp().obj_list()->load_mesh_object(mesh, from_u8(name));
        wxGetApp().mainframe->update_title();
        return object_json(wxGetApp().plater()->model().objects.size() - 1);
    });
}

static json add_shape(const json &args, const std::atomic_bool &running)
{
    const std::string shape = args.value("shape", "");
    const json        size  = args.value("size", json());
    if (!size.is_array() || size.size() != 3 ||
        !std::all_of(size.begin(), size.end(), [](const json &v) { return v.is_number() && v.get<double>() > 0.; }))
        throw std::invalid_argument("size must be [x, y, z] in mm, all > 0");
    const Vec3f s(size[0].get<float>(), size[1].get<float>(), size[2].get<float>());

    TriangleMesh mesh;
    if (shape == "box")
        mesh = make_cube(s.x(), s.y(), s.z());
    else if (shape == "cylinder")
        mesh = make_cylinder(0.5, 1.);
    else if (shape == "sphere")
        mesh = make_sphere(0.5, PI / 90.);
    else
        throw std::invalid_argument("shape must be box, cylinder or sphere");
    if (shape != "box")
        mesh.scale(s);

    return add_mesh_object(running, mesh, args.value("name", shape));
}

// shortcut: needs openscad on PATH, add a setting for its location if that is not enough.
static json render_openscad(const json &args, const std::atomic_bool &running)
{
    namespace fs = boost::filesystem;
    namespace bp = boost::process;

    const std::string code = args.value("code", "");
    if (code.empty())
        throw std::invalid_argument("code is empty");
    const fs::path openscad = bp::search_path("openscad");
    if (openscad.empty())
        throw std::runtime_error("OpenSCAD is not installed (on Debian/Ubuntu: sudo apt install openscad)");

    const fs::path dir = fs::temp_directory_path() / fs::unique_path("bambu-mcp-%%%%-%%%%-%%%%");
    fs::create_directories(dir);
    std::shared_ptr<void> remove_dir(nullptr, [dir](void *) { boost::system::error_code ec; fs::remove_all(dir, ec); });
    const fs::path scad = dir / "model.scad", stl = dir / "model.stl", log = dir / "openscad.log";
    std::ofstream(scad.string()) << code;

    bp::child  child(openscad, "-o", stl.string(), scad.string(), bp::std_out > bp::null, bp::std_err > log);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (child.running()) {
        if (!running || std::chrono::steady_clock::now() > deadline) {
            child.terminate();
            throw std::runtime_error(running ? "OpenSCAD did not finish within 120 s" : "BambuStudio is shutting down");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    child.wait();

    std::stringstream output;
    output << std::ifstream(log.string()).rdbuf();
    TriangleMesh mesh;
    if (child.exit_code() != 0 || !mesh.ReadSTLFile(stl.string().c_str(), true) || mesh.empty())
        throw std::runtime_error("OpenSCAD failed:\n" + output.str());

    json result = add_mesh_object(running, mesh, args.value("name", "openscad"));
    result["openscad_output"] = output.str(); // echo() and warnings, useful for debugging the code
    return result;
}

// Returns an MCP tool result (not plain JSON) so the client receives an image.
static json get_view(const json &args, const std::atomic_bool &running)
{
    static const std::map<std::string, Camera::ViewAngleType> views = {
        {"iso", Camera::ViewAngleType::Iso}, {"top", Camera::ViewAngleType::Top},   {"front", Camera::ViewAngleType::Front},
        {"left", Camera::ViewAngleType::Left}, {"right", Camera::ViewAngleType::Right}, {"rear", Camera::ViewAngleType::Rear}};
    auto view = views.find(args.value("view", "iso"));
    if (view == views.end())
        throw std::invalid_argument("view must be iso, top, front, left, right or rear");
    const unsigned int size = args.value("size", 512);
    if (size < 64 || size > 2048)
        throw std::invalid_argument("size must be between 64 and 2048");

    return run_on_ui_thread(running, [angle = view->second, size] {
        Plater *plater = wxGetApp().plater();
        // Framed on the objects (use_plate_box = false) so small parts are not lost on a big plate.
        const ThumbnailsParams params{{}, false, true, true, false, plater->get_partplate_list().get_curr_plate_index(), false};
        ThumbnailData          data;
        plater->canvas3D()->render_thumbnail(data, size, size, params, Camera::EType::Ortho, angle);
        if (!data.is_valid())
            throw std::runtime_error("Rendering failed");

        size_t png_size = 0;
        void  *png      = tdefl_write_image_to_png_file_in_memory_ex(data.pixels.data(), data.width, data.height, 4, &png_size, MZ_DEFAULT_LEVEL, 1);
        if (png == nullptr)
            throw std::runtime_error("PNG encoding failed");
        std::string base64(boost::beast::detail::base64::encoded_size(png_size), '\0');
        base64.resize(boost::beast::detail::base64::encode(base64.data(), png, png_size));
        mz_free(png);

        json content = json::array();
        content.push_back({{"type", "image"}, {"data", base64}, {"mimeType", "image/png"}});
        return json{{"content", content}, {"isError", false}};
    });
}

static json import_model(const json &args, const std::atomic_bool &running)
{
    const boost::filesystem::path path(args.value("path", ""));
    if (!path.is_absolute() || !boost::filesystem::is_regular_file(path))
        throw std::invalid_argument("path must be an absolute path to an existing file");

    return run_on_ui_thread(running, [path] {
        // Model only: a 3MF's printer and filament settings must not replace the user's presets.
        std::vector<size_t> idxs = wxGetApp().plater()->load_files(std::vector<std::string>{path.string()}, LoadStrategy::LoadModel);
        if (idxs.empty())
            throw std::runtime_error("Could not load " + path.string());
        json objects = json::array();
        for (size_t i : idxs)
            objects.push_back(object_json(i));
        return json{{"objects", objects}};
    });
}

static json delete_objects(const json &args, const std::atomic_bool &running)
{
    const json names = args.value("names", json());
    if (!names.is_null() && !(names.is_array() && std::all_of(names.begin(), names.end(), [](const json &n) { return n.is_string(); })))
        throw std::invalid_argument("names must be an array of object names");

    return run_on_ui_thread(running, [names] {
        Plater *plater = wxGetApp().plater();
        if (names.is_null()) {
            plater->delete_all_objects_from_model();
            return json{{"deleted", "all"}};
        }
        const ModelObjectPtrs &objects = plater->model().objects;
        std::vector<size_t>    idxs;
        for (const json &name : names) {
            auto it = std::find_if(objects.begin(), objects.end(), [&name](const ModelObject *o) { return o->name == name.get<std::string>(); });
            if (it == objects.end())
                throw std::invalid_argument("No object named " + name.get<std::string>());
            idxs.push_back(it - objects.begin());
        }
        // Highest index first, so the remaining indices stay valid.
        std::sort(idxs.rbegin(), idxs.rend());
        idxs.erase(std::unique(idxs.begin(), idxs.end()), idxs.end());
        Plater::TakeSnapshot snapshot(plater, "Delete Objects");
        for (size_t idx : idxs) {
            // Plater::remove() keeps the object list in sync; unlike delete_object_from_model() it asks nothing about cut objects.
            if (objects[idx]->is_cut())
                wxGetApp().obj_list()->invalidate_cut_info_for_object(int(idx));
            plater->remove(idx);
        }
        return json{{"deleted", names}};
    });
}

static json arrange(const json &, const std::atomic_bool &running)
{
    run_on_ui_thread(running, [] {
        Plater *plater = wxGetApp().plater();
        // Claims m_arrange_running; ArrangeJob::finalize() clears it once the new positions are applied.
        if (plater->is_any_job_running() || !plater->last_arrange_job_is_finished())
            throw std::runtime_error("Another arrange or orient job is running");
        plater->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
        plater->arrange();
        return json();
    });
    wait_until(running, [] { return !wxGetApp().plater()->m_arrange_running.load(); }, 120, "Arrange");
    return get_scene(json(), running);
}

static json slice(const json &, const std::atomic_bool &running)
{
    run_on_ui_thread(running, [] {
        Plater *plater = wxGetApp().plater();
        plater->exit_gizmo();
        plater->update(true, true);
        bool model_fits = false, validate_error = false;
        plater->validate_current_plate(model_fits, validate_error);
        if (!model_fits)
            throw std::runtime_error("Some objects are outside the plate; call arrange first");
        if (validate_error)
            throw std::runtime_error("The plate is not valid for slicing, see the notifications in BambuStudio");
        // Same handler as the Slice button, without the button's first-use pop-ups.
        SimpleEvent evt(EVT_GLTOOLBAR_SLICE_PLATE);
        plater->GetEventHandler()->ProcessEvent(evt);
        return json();
    });
    wait_until(running, [] { return !wxGetApp().plater()->is_background_process_slicing(); }, 600, "Slicing");

    return run_on_ui_thread(running, [] {
        PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        if (!plate->is_slice_result_valid())
            throw std::runtime_error("Slicing failed, see the notifications in BambuStudio");
        const PrintStatistics &stats = plate->fff_print()->print_statistics();
        return json{{"print_time", stats.estimated_normal_print_time},
                    {"filament_g", stats.total_weight},
                    {"filament_m", stats.total_used_filament / 1000.}};
    });
}

struct McpTool
{
    const char                                                *name;
    const char                                                *description;
    json                                                       input_schema;
    std::function<json(const json &, const std::atomic_bool &)> run;
};

static const json NO_ARGS = {{"type", "object"}, {"properties", json::object()}};

static const std::vector<McpTool> &tools()
{
    static const std::vector<McpTool> tools = {
        {"get_scene", "List the objects on the build plate, with the bounding box (min, max, size in mm) of each instance.",
         NO_ARGS, get_scene},
        {"add_shape", "Add a box, cylinder or sphere scaled to the given size (mm). It is placed on a free spot of the plate.",
         {{"type", "object"},
          {"properties",
           {{"shape", {{"type", "string"}, {"enum", json::array({"box", "cylinder", "sphere"})}}},
            {"size", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                      {"description", "[x, y, z] in mm; a cylinder or sphere with x != y is elliptic"}}},
            {"name", {{"type", "string"}}}}},
          {"required", json::array({"shape", "size"})}},
         add_shape},
        {"render_openscad",
         "Render OpenSCAD code to a mesh and add it as one object on a free spot of the plate. Units are mm, Z is up. "
         "Use this for any shape add_shape cannot make, and build multi-part parts (e.g. a tray with walls) as one model with union().",
         {{"type", "object"},
          {"properties", {{"code", {{"type", "string"}, {"description", "OpenSCAD source"}}}, {"name", {{"type", "string"}}}}},
          {"required", json::array({"code"})}},
         render_openscad},
        {"get_view", "Render a picture of the objects on the current plate (orthographic), to check what was built.",
         {{"type", "object"},
          {"properties",
           {{"view", {{"type", "string"}, {"enum", json::array({"iso", "top", "front", "left", "right", "rear"})}, {"default", "iso"}}},
            {"size", {{"type", "integer"}, {"description", "Image width and height in pixels, 64-2048"}, {"default", 512}}}}}},
         get_view},
        {"import_model", "Load a model file (STL, 3MF, OBJ, STEP...) onto the plate. Only the geometry is loaded, not printer settings.",
         {{"type", "object"},
          {"properties", {{"path", {{"type", "string"}, {"description", "Absolute path"}}}}},
          {"required", json::array({"path"})}},
         import_model},
        {"delete_objects", "Delete objects by name (the first object with each name). Without names, clear the plate. Undoable with Ctrl+Z.",
         {{"type", "object"}, {"properties", {{"names", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}},
         delete_objects},
        {"arrange", "Arrange all objects on the plate and return the new scene.", NO_ARGS, arrange},
        {"slice", "Slice the current plate with the selected presets. Returns print time and filament use.", NO_ARGS, slice},
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
        if (!args.is_object())
            return rpc_error(id, -32602, "arguments must be an object");
        try {
            const json out = tool->run(args, running);
            return rpc_result(id, out.contains("content") ? out : tool_result(out.dump(2), false));
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
