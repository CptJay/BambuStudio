#include "McpServer.hpp"

#include <chrono>
#include <cstring>
#include <functional>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
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
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/PostProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Emboss.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include <wx/settings.h>
#include <wx/fontenum.h>
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "Tab.hpp"
#include "GUI_Factories.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "ObjectDataViewModel.hpp"
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

// Tools find objects by name, so new objects get one nobody else has: "<name>", "<name> 2", "<name> 3"... UI thread only.
static std::string unique_name(const std::string &name, const ModelObject *self = nullptr)
{
    const ModelObjectPtrs &objects = wxGetApp().plater()->model().objects;
    auto taken = [&objects, self](const std::string &n) {
        return std::any_of(objects.begin(), objects.end(), [&n, self](const ModelObject *o) { return o != self && o->name == n; });
    };
    std::string result = name;
    for (int i = 2; taken(result); ++i)
        result = name + " " + std::to_string(i);
    return result;
}

// Renames an object in the model and in the object list. UI thread only.
static void rename_object(size_t idx, const std::string &name)
{
    wxGetApp().plater()->model().objects[idx]->name = name;
    ObjectDataViewModel *list = wxGetApp().obj_list()->GetModel();
    list->SetName(from_u8(name), list->GetItemById(int(idx)));
}

// Adds the mesh as a new object on a free spot of the plate.
static json add_mesh_object(const std::atomic_bool &running, const TriangleMesh &mesh, const std::string &name)
{
    return run_on_ui_thread(running, [mesh, name] {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Add Primitive");
        wxGetApp().obj_list()->load_mesh_object(mesh, from_u8(unique_name(name)));
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
        for (size_t i : idxs) {
            const ModelObject *obj = wxGetApp().plater()->model().objects[i];
            rename_object(i, unique_name(obj->name, obj));
            objects.push_back(object_json(i));
        }
        return json{{"objects", objects}};
    });
}

// Index of the first object with this name. UI thread only.
static size_t find_object(const std::string &name)
{
    const ModelObjectPtrs &objects = wxGetApp().plater()->model().objects;
    auto it = std::find_if(objects.begin(), objects.end(), [&name](const ModelObject *o) { return o->name == name; });
    if (it == objects.end())
        throw std::invalid_argument("No object named " + name);
    return it - objects.begin();
}

// Reads an optional [x, y, z] number array argument.
static std::optional<Vec3d> vec3_arg(const json &args, const char *key)
{
    if (!args.contains(key))
        return std::nullopt;
    const json &v = args[key];
    if (!v.is_array() || v.size() != 3 || !std::all_of(v.begin(), v.end(), [](const json &n) { return n.is_number(); }))
        throw std::invalid_argument(std::string(key) + " must be [x, y, z]");
    return Vec3d(v[0].get<double>(), v[1].get<double>(), v[2].get<double>());
}

// Reads an optional [x, y] number array argument.
static std::optional<Vec2d> vec2_arg(const json &args, const char *key)
{
    if (!args.contains(key))
        return std::nullopt;
    const json &v = args[key];
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number())
        throw std::invalid_argument(std::string(key) + " must be [x, y]");
    return Vec2d(v[0].get<double>(), v[1].get<double>());
}

static json transform_object(const json &args, const std::atomic_bool &running)
{
    const std::string name   = args.value("name", "");
    const auto        rotate = vec3_arg(args, "rotate");
    const auto        scale  = vec3_arg(args, "scale");
    const auto        position = vec2_arg(args, "position");
    if (scale && (scale->array() <= 0.).any())
        throw std::invalid_argument("scale factors must be > 0");
    if (!rotate && !scale && !position)
        throw std::invalid_argument("give at least one of position, rotate, scale");

    return run_on_ui_thread(running, [name, rotate, scale, position] {
        Plater        *plater   = wxGetApp().plater();
        const size_t   idx      = find_object(name);
        ModelObject   *obj      = plater->model().objects[idx];
        ModelInstance *instance = obj->instances[0];
        Plater::TakeSnapshot snapshot(plater, "Move Object");

        // Scale and rotate first: they change the bounding box the position is measured on.
        if (scale)
            instance->set_scaling_factor(instance->get_scaling_factor().cwiseProduct(*scale));
        if (rotate) {
            const Vec3d r = *rotate * PI / 180.;
            instance->rotate((Eigen::AngleAxisd(r.z(), Vec3d::UnitZ()) * Eigen::AngleAxisd(r.y(), Vec3d::UnitY()) *
                              Eigen::AngleAxisd(r.x(), Vec3d::UnitX())).toRotationMatrix());
        }
        obj->invalidate_bounding_box();
        if (position) {
            const Vec3d center = obj->instance_bounding_box(0).center();
            instance->set_offset(instance->get_offset() + Vec3d(position->x() - center.x(), position->y() - center.y(), 0.));
        }
        // changed_object() keeps a partly sunk object sunk (the app's sinking feature), so put it on the bed first.
        obj->ensure_on_bed(false);
        plater->changed_object(*obj);
        return object_json(idx);
    });
}

static json duplicate_object(const json &args, const std::atomic_bool &running)
{
    const std::string name   = args.value("name", "");
    const int         copies = args.value("copies", 1);
    if (copies < 1 || copies > 100)
        throw std::invalid_argument("copies must be between 1 and 100");

    return run_on_ui_thread(running, [name, copies] {
        Plater      *plater = wxGetApp().plater();
        const size_t idx    = find_object(name);
        const size_t first  = plater->model().objects.size();
        // The app's own Clone: copies keep the original's settings and go on free spots of the plate.
        Selection &selection = plater->canvas3D()->get_selection();
        selection.add_object(unsigned(idx), true);
        selection.clone(copies);

        // Unique names, so later tool calls can tell the copies apart.
        json objects = json::array();
        for (size_t i = first; i < plater->model().objects.size(); ++i) {
            rename_object(i, unique_name(name, plater->model().objects[i]));
            objects.push_back(object_json(i));
        }
        return json{{"objects", objects}};
    });
}

// Text as a flat mesh lying on Z = 0, centered on the origin, built like the Text gizmo's mesh. UI thread only (wxFont).
static TriangleMesh text_mesh(const std::string &text, const std::string &font, double size, double height)
{
    // Font lookup falls back to a similar font for unknown names, so check the name exists first.
    if (!font.empty() && !wxFontEnumerator::IsValidFacename(font))
        throw std::invalid_argument("Font not installed: " + font);
    const wxFont wx_font = font.empty() ? wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT) : wxFont(wxFontInfo(10).FaceName(font));
    std::unique_ptr<Emboss::FontFile> file = WxFontUtils::create_font_file(wx_font);
    if (!file)
        throw std::runtime_error("Font not found: " + font);
    Emboss::FontFileWithCache font_with_cache(std::move(file));
    const FontProp            prop{float(size)};
    const double              scale = Emboss::get_text_shape_scale(prop, *font_with_cache.font_file);
    EmbossShape               shape;
    const ExPolygons          shapes = Emboss::text2shapes(shape, font_with_cache, text.c_str(), prop, scale).expolygons;
    if (shapes.empty())
        throw std::runtime_error("The font has no glyphs for this text");

    // Shapes are in font units; the projection scales them to mm and extrudes them by height.
    Emboss::ProjectTransform project(std::make_unique<Emboss::ProjectZ>(height / scale), Transform3d(Eigen::Scaling(scale)));
    TriangleMesh mesh(Emboss::polygons2model(shapes, project));
    const BoundingBoxf3 bb = mesh.bounding_box();
    mesh.translate(-bb.center().x(), -bb.center().y(), -bb.min.z());
    return mesh;
}

// shortcut: text lies flat on the surface found straight below the position, so it suits upward-facing flat faces only;
// rotate the object first for other faces, or use the Text gizmo for curved ones.
static json add_text(const json &args, const std::atomic_bool &running)
{
    const std::string name   = args.value("name", "");
    const std::string text   = args.value("text", "");
    const std::string font   = args.value("font", "");
    const std::string mode   = args.value("mode", "engrave");
    const double      size   = args.value("size", 10.);
    const double      depth  = args.value("depth", 1.);
    const double      angle  = args.value("rotation", 0.);
    if (text.empty())
        throw std::invalid_argument("text is empty");
    if (mode != "engrave" && mode != "emboss")
        throw std::invalid_argument("mode must be engrave or emboss");
    if (size <= 0. || depth <= 0.)
        throw std::invalid_argument("size and depth must be > 0");
    const auto position = vec2_arg(args, "position");

    return run_on_ui_thread(running, [=] {
        Plater       *plater = wxGetApp().plater();
        const size_t  idx    = find_object(name);
        ModelObject  *obj    = plater->model().objects[idx];
        const BoundingBoxf3 bb = obj->instance_bounding_box(0);
        const Vec2d   xy     = position.value_or(Vec2d(bb.center().x(), bb.center().y()));

        // Find the surface height under the text by casting a ray down onto the object.
        const TriangleMesh         world_mesh = obj->mesh(); // AABBMesh keeps a pointer to it, so it must outlive the query
        const AABBMesh             surface(world_mesh);
        const AABBMesh::hit_result hit = surface.query_ray_hit(Vec3d(xy.x(), xy.y(), bb.max.z() + 1.), -Vec3d::UnitZ());
        if (!hit.is_hit())
            throw std::invalid_argument("position is not over the object");
        const double top = hit.position().z();

        // A little extra height that passes the surface, so the cut is clean or the raised text is fused to the part.
        const double overlap = 0.2;
        TriangleMesh mesh    = text_mesh(text, font, size, depth + overlap);
        const bool   engrave = mode == "engrave";
        mesh.transform(Transform3d(Eigen::Translation3d(xy.x(), xy.y(), engrave ? top - depth : top - overlap) *
                                   Eigen::AngleAxisd(angle * PI / 180., Vec3d::UnitZ())));
        // Volumes are stored in object coordinates.
        // true: flip the triangles back when the instance is mirrored, so the volume is not inside out.
        mesh.transform(obj->instances[0]->get_matrix().inverse(), true);

        Plater::TakeSnapshot snapshot(plater, "Add Text");
        ModelVolume *volume = obj->add_volume(std::move(mesh), engrave ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART);
        volume->name        = text;
        // Same extruder rule as ObjectList::load_generic_subobject().
        volume->config.set_key_value("extruder", new ConfigOptionInt(!engrave && obj->config.has("extruder") ? obj->config.opt_int("extruder") : 0));
        wxGetApp().obj_list()->reorder_volumes_and_get_selection(int(idx));
        plater->changed_object(*obj);
        return object_json(idx);
    });
}

// Shown by get_settings when no keys are asked for.
static const std::vector<std::string> COMMON_SETTINGS = {
    "layer_height",    "initial_layer_print_height", "wall_loops", "top_shell_layers", "bottom_shell_layers", "sparse_infill_density",
    "sparse_infill_pattern", "enable_support",       "support_type", "brim_type",      "ironing_type",        "print_sequence"};

static std::string preset_label(const PresetCollection &presets)
{
    return presets.get_edited_preset().name + (presets.current_is_dirty() ? " (modified)" : "");
}

// Parses a JSON value into the option for key, checked like the settings panel does. Throws for unknown keys and bad values.
static ConfigOption *parse_setting(const std::string &key, const json &value, const std::vector<std::string> &allowed)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr || std::find(allowed.begin(), allowed.end(), key) == allowed.end())
        throw std::invalid_argument("Not a process setting that can be changed here: " + key);
    // Shell commands run after slicing; a tool must never be able to set them.
    if (key == "post_process")
        throw std::invalid_argument("post_process can only be set in BambuStudio");
    const std::string text = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "1" : "0") : value.dump();
    std::unique_ptr<ConfigOption> opt(def->create_empty_option());
    if (!opt->deserialize(text))
        throw std::invalid_argument("Invalid value for " + key + ": " + text);
    if (def->type == coFloat || def->type == coInt || def->type == coPercent) {
        const double number = def->type == coInt ? opt->getInt() : opt->getFloat(); // getFloat() throws for ints
        if (number < def->min || number > def->max)
            throw std::invalid_argument(key + " must be between " + std::to_string(def->min) + " and " + std::to_string(def->max));
    }
    return opt.release();
}

static json get_settings(const json &args, const std::atomic_bool &running)
{
    const json keys = args.value("keys", json::array());
    if (!keys.is_array() || !std::all_of(keys.begin(), keys.end(), [](const json &k) { return k.is_string(); }))
        throw std::invalid_argument("keys must be an array of setting names");
    const std::string name = args.value("name", "");

    return run_on_ui_thread(running, [keys, name] {
        PresetBundle             &bundle = *wxGetApp().preset_bundle;
        const DynamicPrintConfig &plate  = *wxGetApp().plater()->get_partplate_list().get_curr_plate()->config();
        DynamicPrintConfig        config = bundle.full_config();
        config.apply(plate); // the plate's own settings win, as in BackgroundSlicingProcess::apply()
        const ModelObject        *obj    = name.empty() ? nullptr : wxGetApp().plater()->model().objects[find_object(name)];

        json settings = json::object();
        for (const std::string &key : keys.empty() ? COMMON_SETTINGS : keys.get<std::vector<std::string>>()) {
            const bool          from_object = obj != nullptr && obj->config.has(key);
            const ConfigOption *opt         = from_object ? obj->config.option(key) : config.option(key);
            if (opt == nullptr)
                throw std::invalid_argument("Unknown setting: " + key);
            settings[key] = {{"value", opt->serialize()},
                             {"label", print_config_def.get(key)->label},
                             {"source", from_object ? "object" : plate.has(key) ? "plate" : "preset"}};
        }
        return json{{"presets", {{"printer", preset_label(bundle.printers)},
                                 {"process", preset_label(bundle.prints)},
                                 {"filaments", bundle.filament_presets}}},
                    {"settings", settings}};
    });
}

static json set_settings(const json &args, const std::atomic_bool &running)
{
    const json values = args.value("values", json::object());
    if (!values.is_object() || values.empty())
        throw std::invalid_argument("values must be an object like {\"wall_loops\": 3}");
    const std::string name = args.value("name", "");

    const json requested = run_on_ui_thread(running, [values, name] {
        // Parse everything first, so a bad value changes nothing.
        DynamicPrintConfig changes;
        json               requested = json::object();
        const std::vector<std::string> allowed = name.empty() ? Preset::print_options() : SettingsFactory::get_options(false);
        for (const auto &[key, value] : values.items()) {
            changes.set_key_value(key, parse_setting(key, value, allowed));
            requested[key] = changes.option(key)->serialize();
        }
        // shortcut: ConfigManipulation shows a modal dialog for some invalid values (zero first layer, huge elephant foot...);
        // the call waits until it is clicked. Only layer_height is caught here.
        if (changes.has("layer_height") && (changes.opt_float("layer_height") <= 0. || changes.opt_float("layer_height") > 0.6))
            throw std::invalid_argument("layer_height must be above 0 and at most 0.6 mm");

        Plater *plater = wxGetApp().plater();
        if (name.empty()) {
            // Same path as editing the process settings panel; the preset shows as modified.
            wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(changes);
        } else {
            const size_t idx = find_object(name);
            ModelObject *obj = plater->model().objects[idx];
            Plater::TakeSnapshot snapshot(plater, "Object settings changed");
            for (const std::string &key : changes.keys())
                obj->config.set_key_value(key, changes.option(key)->clone());
            ObjectList *list = wxGetApp().obj_list();
            list->add_settings_item(list->GetModel()->GetItemById(int(idx)), &obj->config.get());
            plater->changed_object(*obj);
        }
        return requested;
    });
    json keys = json::array();
    for (const auto &item : values.items())
        keys.push_back(item.key());
    json result = get_settings({{"keys", keys}, {"name", name}}, running);
    // The panel's own rules can change a value right after it is set; say so instead of reporting success.
    for (auto &[key, entry] : result["settings"].items())
        if (entry["value"] != requested[key])
            entry["changed_by_bambustudio"] = "asked for " + requested[key].get<std::string>();
    return result;
}

// shortcut: process presets only; printer and filament presets have dialogs and AMS slots, so they are still chosen in the app.
static json select_process_preset(const json &args, const std::atomic_bool &running)
{
    const std::string name = args.value("preset", "");
    return run_on_ui_thread(running, [name] {
        PresetCollection &prints = wxGetApp().preset_bundle->prints;
        // Switching would open the "unsaved changes" dialog, or lose the user's edits.
        if (prints.current_is_dirty())
            throw std::runtime_error("The process preset has unsaved changes. Save or discard them in BambuStudio first, "
                                     "or keep this preset and use set_settings.");
        const Preset *preset = prints.find_preset(name, false);
        if (preset == nullptr || !preset->is_visible || !preset->is_compatible) {
            std::string available;
            for (const Preset &p : prints)
                if (p.is_visible && p.is_compatible)
                    available += (available.empty() ? "" : ", ") + p.name;
            throw std::invalid_argument("No process preset named \"" + name + "\" for this printer. Available: " + available);
        }
        wxGetApp().get_tab(Preset::TYPE_PRINT)->select_preset(name);
        wxGetApp().plater()->on_config_change(wxGetApp().preset_bundle->full_config());
        return json{{"process", preset_label(prints)}};
    });
}

// Checks a project path from a tool: absolute, a plain .3mf (a .gcode.3mf opens in preview-only mode), and no characters
// the app refuses with a warning dialog (the path is shown in the home page's recent list).
static boost::filesystem::path project_path_arg(const json &args)
{
    const boost::filesystem::path path(args.value("path", ""));
    const std::string             text = path.string();
    if (!path.is_absolute() || !boost::iends_with(text, ".3mf") || boost::iends_with(text, ".gcode.3mf"))
        throw std::invalid_argument("path must be an absolute path ending in .3mf (not .gcode.3mf)");
    if (Plater::has_html_unsafe_path_characters(text))
        throw std::invalid_argument("path must not contain quotes or angle brackets");
    return path;
}

static json save_project(const json &args, const std::atomic_bool &running)
{
    namespace fs = boost::filesystem;
    const fs::path path = project_path_arg(args);
    if (!fs::is_directory(path.parent_path()))
        throw std::invalid_argument("Folder does not exist: " + path.parent_path().string());
    if (fs::exists(path) && !args.value("overwrite", false))
        throw std::invalid_argument("File already exists: " + path.string() + " (set overwrite to replace it)");

    return run_on_ui_thread(running, [path] {
        Plater        *plater   = wxGetApp().plater();
        const wxString previous = plater->get_project_filename(".3mf");
        // With a project file name set, the app's own Save writes there without a file dialog.
        plater->set_project_filename(from_u8(path.string()));
        if (plater->save_project() != wxID_YES) {
            plater->set_project_filename(previous);
            throw std::runtime_error("Failed to save " + path.string());
        }
        return json{{"path", path.string()}};
    });
}

static json open_project(const json &args, const std::atomic_bool &running)
{
    const boost::filesystem::path path = project_path_arg(args);
    if (!boost::filesystem::is_regular_file(path))
        throw std::invalid_argument("No such file: " + path.string());

    run_on_ui_thread(running, [path] {
        Plater *plater = wxGetApp().plater();
        // Otherwise the app asks to save first; refuse rather than wait on that dialog or lose the user's work.
        if (!plater->up_to_date(false, false) || wxGetApp().has_current_preset_changes())
            throw std::runtime_error("The current project has unsaved changes. Use save_project first, "
                                     "or save or discard them in BambuStudio.");
        if (plater->load_project(from_u8(path.string())) == wxID_CANCEL)
            throw std::runtime_error("Could not open " + path.string());
        return json();
    });
    json scene       = get_scene(json(), running);
    scene["project"] = path.string();
    return scene;
}

// Plate coordinates, so an OpenSCAD import() lines up with the bounding boxes from get_scene.
static json export_object(const json &args, const std::atomic_bool &running)
{
    namespace fs = boost::filesystem;
    const std::string name = args.value("name", "");
    const fs::path    path(args.value("path", ""));
    if (!path.is_absolute() || !boost::iends_with(path.string(), ".stl"))
        throw std::invalid_argument("path must be an absolute path ending in .stl");
    if (!fs::is_directory(path.parent_path()))
        throw std::invalid_argument("Folder does not exist: " + path.parent_path().string());
    if (fs::exists(path) && !args.value("overwrite", false))
        throw std::invalid_argument("File already exists: " + path.string() + " (set overwrite to replace it)");

    return run_on_ui_thread(running, [name, path] {
        const size_t idx  = find_object(name);
        TriangleMesh mesh = wxGetApp().plater()->model().objects[idx]->mesh();
        if (!mesh.write_binary(path.string().c_str()))
            throw std::runtime_error("Failed to write " + path.string());
        json result    = object_json(idx);
        result["path"] = path.string();
        return result;
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
        for (const json &name : names)
            idxs.push_back(find_object(name.get<std::string>()));
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

static json undo(const json &, const std::atomic_bool &running)
{
    // The app only undoes in the 3D editor tab, and slice leaves it on the Preview tab.
    // The switch completes in later UI events, so undo in a separate step.
    run_on_ui_thread(running, [] {
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
        wxGetApp().plater()->select_view_3D("3D");
        return json();
    });
    const json undone = run_on_ui_thread(running, [] {
        Plater *plater = wxGetApp().plater();
        if (!plater->can_undo())
            throw std::runtime_error("Nothing to undo");
        std::string name;
        plater->undo_redo_topmost_string_getter(true, name);
        plater->undo();
        return json(name);
    });
    json result      = get_scene(json(), running);
    result["undone"] = undone;
    return result;
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

// The app only shows a slicing error as a notification, so McpServer::start() records the last one here. UI thread only.
static std::string last_slicing_error;

// Current warnings of the finished slice (e.g. floating regions, missing supports), from the print and from each object.
static json slicing_warnings(const Print &print)
{
    json warnings = json::array();
    auto add      = [&warnings](const PrintStateBase::StateWithWarnings &state, const std::string &object) {
        for (const PrintStateBase::Warning &w : state.warnings)
            // Several slicing steps can raise the same warning.
            if (w.current && std::none_of(warnings.begin(), warnings.end(), [&w](const json &e) { return e["message"] == w.message; })) {
                json entry = {{"message", w.message}, {"critical", w.level == PrintStateBase::WarningLevel::CRITICAL}};
                if (!object.empty())
                    entry["object"] = object;
                warnings.push_back(entry);
            }
    };
    for (int step = 0; step < psCount; ++step)
        add(print.step_state_with_warnings(PrintStep(step)), "");
    for (const PrintObject *obj : print.objects())
        for (int step = 0; step < posCount; ++step)
            add(obj->step_state_with_warnings(PrintObjectStep(step)), obj->model_object()->name);
    StringObjectException check_warning;
    print.validate(&check_warning);
    if (!check_warning.string.empty())
        warnings.push_back({{"message", check_warning.string}, {"critical", false}});
    return warnings;
}

static json slice(const json &, const std::atomic_bool &running)
{
    run_on_ui_thread(running, [] {
        Plater *plater = wxGetApp().plater();
        // A project opened by a tool can bring shell commands that run after slicing, and the app skips its warning when
        // the user once ticked "remember my choice". Tools never slice those; the user does it in BambuStudio.
        const std::vector<std::string> scripts = wxGetApp().preset_bundle->full_config().option<ConfigOptionStrings>("post_process")->values;
        if (std::any_of(scripts.begin(), scripts.end(), [](const std::string &s) { return !boost::trim_copy(s).empty(); }))
            throw std::runtime_error("This project has post-processing scripts; slice it in BambuStudio instead");
        plater->exit_gizmo();
        plater->update(true, true);
        bool model_fits = false, validate_error = false;
        plater->validate_current_plate(model_fits, validate_error);
        if (!model_fits)
            throw std::runtime_error("Some objects do not fit the printer: outside the plate (call arrange) or taller than the build height");
        if (validate_error) {
            const std::string why = plater->get_partplate_list().get_curr_plate()->fff_print()->validate().string;
            throw std::runtime_error("The plate cannot be sliced: " + (why.empty() ? "see the notifications in BambuStudio" : why));
        }
        last_slicing_error.clear();
        // Same handler as the Slice button, without the button's first-use pop-ups.
        SimpleEvent evt(EVT_GLTOOLBAR_SLICE_PLATE);
        plater->GetEventHandler()->ProcessEvent(evt);
        return json();
    });
    wait_until(running, [] { return !wxGetApp().plater()->is_background_process_slicing(); }, 600, "Slicing");

    return run_on_ui_thread(running, [] {
        PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
        if (!plate->is_slice_result_valid())
            throw std::runtime_error("Slicing failed: " + (last_slicing_error.empty() ? "see the notifications in BambuStudio" : last_slicing_error));
        const PrintStatistics &stats = plate->fff_print()->print_statistics();
        return json{{"print_time", stats.estimated_normal_print_time},
                    {"filament_g", stats.total_weight},
                    {"filament_m", stats.total_used_filament / 1000.},
                    {"warnings", slicing_warnings(*plate->fff_print())}};
    });
}

// Writes a file only; nothing is ever sent to a printer from a tool.
static json export_gcode(const json &args, const std::atomic_bool &running)
{
    namespace fs = boost::filesystem;
    const fs::path path(args.value("path", ""));
    const bool     is_3mf = boost::iends_with(path.string(), ".gcode.3mf");
    if (!path.is_absolute() || !(is_3mf || boost::iends_with(path.string(), ".gcode")))
        throw std::invalid_argument("path must be an absolute path ending in .gcode or .gcode.3mf");
    if (!fs::is_directory(path.parent_path()))
        throw std::invalid_argument("Folder does not exist: " + path.parent_path().string());
    if (fs::exists(path) && !args.value("overwrite", false))
        throw std::invalid_argument("File already exists: " + path.string() + " (set overwrite to replace it)");

    // Re-slicing an up-to-date plate is instant.
    json result = slice(json(), running);

    run_on_ui_thread(running, [path, is_3mf] {
        Plater    *plater = wxGetApp().plater();
        PartPlate *plate  = plater->get_partplate_list().get_curr_plate();
        if (is_3mf) {
            // Same call as File > Export > Export plate sliced file, without its file dialog.
            if (plater->export_3mf(path, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel,
                                   plater->get_partplate_list().get_curr_plate_index()) < 0)
                throw std::runtime_error("Failed to write " + path.string());
        } else {
            // Same as BackgroundSlicingProcess::export_gcode(): the temp G-code is already post-processed.
            std::string error;
            if (copy_file(plate->get_tmp_gcode_path(), path.string(), error) != SUCCESS)
                throw std::runtime_error("Failed to write " + path.string() + ": " + error);
            gcode_add_line_number(path.string(), plate->fff_print()->full_print_config());
        }
        return json();
    });
    result["path"] = path.string();
    return result;
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
         "Use this for any shape add_shape cannot make, and build multi-part parts (e.g. a tray with walls) as one model with union(). "
         "To change an existing object (engrave text, drill a hole...), export_object it, import() the STL here, combine with "
         "difference() or union(), then delete the old object.",
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
        {"transform_object",
         "Move, rotate and/or scale an object, then drop it onto the bed. Order: scale, rotate, then position.",
         {{"type", "object"},
          {"properties",
           {{"name", {{"type", "string"}}},
            {"position", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2},
                          {"description", "[x, y] in mm: where the center of the object's bounding box goes on the plate"}}},
            {"rotate", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                        {"description", "[x, y, z] degrees, added to the current rotation, around the plate axes"}}},
            {"scale", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                       {"description", "[x, y, z] factors multiplied with the current scale, along the object's own axes"}}}}},
          {"required", json::array({"name"})}},
         transform_object},
        {"add_text",
         "Engrave text into, or emboss it onto, the top surface of an object, using a system font. The text becomes a part of the "
         "object (a negative part when engraving), so the user can still move or delete it in BambuStudio. Works on flat, "
         "upward-facing surfaces; rotate the object first to write on another face.",
         {{"type", "object"},
          {"properties",
           {{"name", {{"type", "string"}, {"description", "Object to write on"}}},
            {"text", {{"type", "string"}}},
            {"mode", {{"type", "string"}, {"enum", json::array({"engrave", "emboss"})}, {"default", "engrave"}}},
            {"position", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2},
                          {"description", "[x, y] in mm, center of the text on the plate; default: center of the object"}}},
            {"size", {{"type", "number"}, {"description", "Font size in mm"}, {"default", 10}}},
            {"depth", {{"type", "number"}, {"description", "Engrave depth or emboss height in mm"}, {"default", 1}}},
            {"rotation", {{"type", "number"}, {"description", "Degrees around Z, counterclockwise"}, {"default", 0}}},
            {"font", {{"type", "string"}, {"description", "Font name, e.g. \"DejaVu Sans\"; default: the system UI font"}}}}},
          {"required", json::array({"name", "text"})}},
         add_text},
        {"get_settings",
         "Read the selected presets and print settings. Without keys, returns common ones (layer height, walls, infill, supports, "
         "brim...). Each value's source is \"preset\", \"plate\" (this plate's own settings) or \"object\" (with an object name).",
         {{"type", "object"},
          {"properties",
           {{"keys", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Setting names, e.g. [\"wall_loops\"]"}}},
            {"name", {{"type", "string"}, {"description", "Object name, for per-object settings"}}}}}},
         get_settings},
        {"set_settings",
         "Change process settings (layer height, walls, infill, supports...), for the whole plate or, with an object name, for that "
         "object only. Values are checked like the settings panel does; one bad value changes nothing. Unusual values may open a "
         "BambuStudio dialog that the user must answer. Pick the process preset "
         "first: plate changes mark it as modified, and a modified preset cannot be switched by select_process_preset.",
         {{"type", "object"},
          {"properties",
           {{"values", {{"type", "object"}, {"description", "e.g. {\"wall_loops\": 3, \"sparse_infill_density\": \"25%\", \"enable_support\": true}"}}},
            {"name", {{"type", "string"}, {"description", "Object name; omit for the whole plate"}}}}},
          {"required", json::array({"values"})}},
         set_settings},
        {"select_process_preset",
         "Switch the process preset (e.g. \"0.12mm Fine @BBL X1C\"). An unknown name returns the list of available presets. "
         "Printer and filament presets are chosen in BambuStudio.",
         {{"type", "object"}, {"properties", {{"preset", {{"type", "string"}}}}}, {"required", json::array({"preset"})}},
         select_process_preset},
        {"save_project",
         "Save the whole project (objects, plates, settings) as a BambuStudio .3mf, like File > Save Project As. "
         "It becomes the current project file.",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"description", "Absolute path ending in .3mf"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}}},
          {"required", json::array({"path"})}},
         save_project},
        {"open_project",
         "Open a BambuStudio .3mf project, replacing the current one. Refused while the current project has unsaved changes.",
         {{"type", "object"},
          {"properties", {{"path", {{"type", "string"}, {"description", "Absolute path ending in .3mf"}}}}},
          {"required", json::array({"path"})}},
         open_project},
        {"export_object",
         "Save an object as a binary STL, in plate coordinates (they match get_scene's bounding boxes). "
         "Use it with render_openscad's import() to edit existing objects.",
         {{"type", "object"},
          {"properties",
           {{"name", {{"type", "string"}}},
            {"path", {{"type", "string"}, {"description", "Absolute path ending in .stl"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}}},
          {"required", json::array({"name", "path"})}},
         export_object},
        {"duplicate_object", "Make copies of an object, placed on free spots. Copies get the next free names: \"<name> 2\", \"<name> 3\"...",
         {{"type", "object"},
          {"properties", {{"name", {{"type", "string"}}}, {"copies", {{"type", "integer"}, {"default", 1}}}}},
          {"required", json::array({"name"})}},
         duplicate_object},
        {"undo", "Undo the last change to the plate (add, delete, move, text, object settings...), like Ctrl+Z. "
                 "Returns what was undone and the new scene. Preset and plate-wide setting changes are not undone.",
         NO_ARGS, undo},
        {"arrange", "Arrange all objects on the plate and return the new scene.", NO_ARGS, arrange},
        {"slice", "Slice the current plate with the selected presets. Returns print time, filament use and the slicer's warnings "
                  "(e.g. floating regions, missing supports); if slicing fails, the error says why.",
         NO_ARGS, slice},
        {"export_gcode",
         "Slice the current plate if needed and save it to a file. Use .gcode.3mf for Bambu Lab printers, .gcode for plain G-code. "
         "Does not send anything to a printer.",
         {{"type", "object"},
          {"properties",
           {{"path", {{"type", "string"}, {"description", "Absolute path ending in .gcode or .gcode.3mf"}}},
            {"overwrite", {{"type", "boolean"}, {"default", false}}}}},
          {"required", json::array({"path"})}},
         export_gcode},
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

    // Runs before the plater's own handler (wx calls the last bound one first); Skip() passes the event on unchanged.
    wxGetApp().plater()->Bind(EVT_PROCESS_COMPLETED, [](SlicingProcessCompletedEvent &evt) {
        last_slicing_error = evt.error() ? evt.format_error_message().first : "";
        evt.Skip();
    });

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
