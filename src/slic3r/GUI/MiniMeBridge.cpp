#include "MiniMeBridge.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_ObjectList.hpp"
#include "Tab.hpp"
#include "Gizmos/GLGizmosManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Support/SupportSurroundings.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cctype>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

namespace Slic3r::GUI {

namespace {

using json = nlohmann::json;
namespace http = boost::beast::http;
using tcp      = boost::asio::ip::tcp;

struct ToolError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

std::atomic_bool s_stop{false};
std::atomic_bool s_running{false};
std::atomic_int  s_port{0};
std::atomic_bool s_chat_needs{false};
std::thread      s_thread;
std::mutex       s_status_mutex;
std::string      s_status{"off"};
std::mutex       s_tool_mutex; // one tool at a time

void set_status(const std::string &status)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    s_status = status;
}

// ---------------------------------------------------------------------------------------------------------------------
// helpers

// Runs on the GUI thread and waits for the result (exceptions are passed back).
json on_gui(std::function<json()> fn, int timeout_ms = 60000)
{
    auto task = std::make_shared<std::packaged_task<json()>>(std::move(fn));
    std::future<json> result = task->get_future();
    wxGetApp().CallAfter([task] { (*task)(); });
    for (int waited = 0; result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready; waited += 100)
        if (s_stop || waited >= timeout_ms)
            throw ToolError(s_stop ? "MiniMe is closing" : "MiniMe did not answer in time (a dialog may be open in MiniMe) - try again");
    return result.get();
}

Plater *plater()
{
    Plater *p = wxGetApp().plater();
    if (p == nullptr)
        throw ToolError("MiniMe is not ready yet");
    return p;
}

double r2(double v) { return std::round(v * 100.) / 100.; }
json   xyz(const Vec3d &v) { return json::array({r2(v.x()), r2(v.y()), r2(v.z())}); }
json   xy(const Vec2d &v) { return json::array({r2(v.x()), r2(v.y())}); }

std::string lower(std::string s)
{
    for (char &c : s)
        c = char(std::tolower((unsigned char) c));
    return s;
}

double num(const json &args, const char *key, double def)
{
    if (!args.contains(key) || args[key].is_null())
        return def;
    if (!args[key].is_number())
        throw ToolError(std::string("'") + key + "' must be a number");
    return args[key].get<double>();
}

std::string str(const json &args, const char *key, const std::string &def)
{
    if (!args.contains(key) || args[key].is_null())
        return def;
    if (!args[key].is_string())
        throw ToolError(std::string("'") + key + "' must be text");
    return args[key].get<std::string>();
}

bool flag(const json &args, const char *key, bool def)
{
    if (!args.contains(key) || args[key].is_null())
        return def;
    if (!args[key].is_boolean())
        throw ToolError(std::string("'") + key + "' must be true or false");
    return args[key].get<bool>();
}

// an object by index or name
int find_object(const json &args, const char *key = "object")
{
    Model &model = plater()->model();
    if (!args.contains(key) || args[key].is_null()) {
        if (model.objects.size() == 1)
            return 0;
        throw ToolError("say which object (its index or name; get_scene lists them)");
    }
    const json &v = args[key];
    if (v.is_number_integer()) {
        const int idx = v.get<int>();
        if (idx < 0 || idx >= int(model.objects.size()))
            throw ToolError("there is no object " + std::to_string(idx) + " (there are " + std::to_string(model.objects.size()) + ")");
        return idx;
    }
    if (v.is_string()) {
        const std::string name = v.get<std::string>();
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (model.objects[i]->name == name)
                return int(i);
        int found = -1, count = 0;
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (lower(model.objects[i]->name).find(lower(name)) != std::string::npos) {
                found = int(i);
                ++count;
            }
        if (count == 1)
            return found;
        throw ToolError(count == 0 ? "no object is called '" + name + "'" : "several objects match '" + name + "', use the index");
    }
    throw ToolError("'object' must be an index or a name");
}

int plate_of_instance(int obj_idx, int inst_idx)
{
    PartPlateList &plates = plater()->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i)
        if (plates.get_plate(i)->contain_instance(obj_idx, inst_idx))
            return i;
    return -1;
}

Vec2d plate_origin(int plate_idx)
{
    PartPlateList &plates = plater()->get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        plate_idx = plates.get_curr_plate_index();
    const Vec3d o = plates.get_plate(plate_idx)->get_origin();
    return {o.x(), o.y()};
}

int plate_arg(const json &args)
{
    PartPlateList &plates = plater()->get_partplate_list();
    if (!args.contains("plate") || args["plate"].is_null())
        return plates.get_curr_plate_index();
    const int idx = int(num(args, "plate", 0));
    if (idx < 0 || idx >= plates.get_plate_count())
        throw ToolError("there is no plate " + std::to_string(idx) + " (there are " + std::to_string(plates.get_plate_count()) + ")");
    return idx;
}

// closes an open tool (gizmo), so that its own copy of the paint or the plate zones does not overwrite the change
void close_gizmo()
{
    if (GLCanvas3D *canvas = plater()->get_view3D_canvas3D()) {
        GLGizmosManager &gizmos = canvas->get_gizmos_manager();
        if (gizmos.get_current_type() != GLGizmosManager::Undefined)
            gizmos.reset_all_states();
    }
}

std::string value_text(const json &v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_number())
        return v.dump();
    if (v.is_array()) {
        std::string out;
        for (const json &e : v)
            out += (out.empty() ? "" : ",") + value_text(e);
        return out;
    }
    throw ToolError("a setting value must be text, a number, true/false or a list");
}

std::string base64(const std::string &in)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t n = (uint32_t((unsigned char) in[i]) << 16) | (uint32_t((unsigned char) in[i + 1]) << 8) | uint32_t((unsigned char) in[i + 2]);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];
        out += tbl[n & 63];
    }
    if (i < in.size()) {
        uint32_t n = uint32_t((unsigned char) in[i]) << 16;
        if (i + 1 < in.size())
            n |= uint32_t((unsigned char) in[i + 1]) << 8;
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += i + 1 < in.size() ? tbl[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

std::string format_time(double seconds)
{
    const int s = int(std::round(seconds));
    std::ostringstream out;
    if (s >= 3600)
        out << s / 3600 << "h ";
    out << (s % 3600) / 60 << "m " << s % 60 << "s";
    return out.str();
}

const char *ZONE_KEYS[2] = {"support_plate_block_area", "support_plate_allow_area"};

ExPolygons zone_areas(const ConfigOptionPointsGroups *opt)
{
    Polygons polys;
    if (opt != nullptr)
        for (const Vec2ds &pts : opt->values) {
            if (pts.size() < 3)
                continue;
            Polygon poly;
            for (const Vec2d &pt : pts)
                poly.points.emplace_back(coord_t(scale_(pt.x())), coord_t(scale_(pt.y())));
            polys.emplace_back(std::move(poly));
        }
    return union_ex(polys);
}

double area_mm2(const ExPolygons &areas)
{
    double a = 0.;
    for (const ExPolygon &ex : areas)
        a += ex.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// ---------------------------------------------------------------------------------------------------------------------
// tools

json tool_get_scene(const json &)
{
    return on_gui([]() -> json {
        Plater             *p      = plater();
        Model              &model  = p->model();
        PartPlateList      &plates = p->get_partplate_list();
        PresetBundle       *pb     = wxGetApp().preset_bundle;
        const DynamicPrintConfig full = pb->full_config();

        json j;
        j["current_plate"] = plates.get_curr_plate_index();
        j["view"]          = p->is_preview_shown() ? "preview" : "3d";
        j["printer"]       = pb->printers.get_edited_preset().name;
        j["process"]       = pb->prints.get_edited_preset().name + (pb->prints.get_edited_preset().is_dirty ? " (modified, not saved)" : "");
        j["filaments"]     = pb->filament_presets;
        if (const auto *area = full.option<ConfigOptionPoints>("printable_area"); area != nullptr && !area->values.empty()) {
            BoundingBoxf bb;
            for (const Vec2d &pt : area->values)
                bb.merge(pt);
            j["plate_size_mm"] = xy(bb.size());
        }
        if (full.has("printable_height"))
            j["printable_height_mm"] = full.opt_float("printable_height");
        json settings;
        for (const char *key : {"layer_height", "enable_support", "support_type", "support_style", "support_threshold_angle", "support_on_build_plate_only",
                                "tree_support_branch_diameter", "wall_loops", "sparse_infill_density", "sparse_infill_pattern", "brim_type", "print_sequence"})
            if (full.has(key))
                settings[key] = full.opt_serialize(key);
        j["key_process_settings"] = settings;

        json jplates = json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate *plate = plates.get_plate(i);
            json       jp;
            jp["index"]     = i;
            jp["origin_mm"] = xy(plate_origin(i));
            jp["sliced"]    = plate->is_slice_result_valid();
            json zones;
            const char *names[2] = {"no_supports", "supports_only_here"};
            for (int z = 0; z < 2; ++z) {
                const ExPolygons areas = zone_areas(plate->config()->option<ConfigOptionPointsGroups>(ZONE_KEYS[z]));
                if (!areas.empty())
                    zones[names[z]] = {{"areas", areas.size()}, {"total_mm2", r2(area_mm2(areas))}, {"bbox", [&] {
                                           const BoundingBox bb = get_extents(areas);
                                           return json::array({r2(unscale<double>(bb.min.x())), r2(unscale<double>(bb.min.y())),
                                                               r2(unscale<double>(bb.max.x())), r2(unscale<double>(bb.max.y()))});
                                       }()}};
            }
            jp["support_zones"] = zones.is_null() ? json::object() : zones;
            json objs = json::array();
            for (const auto &oi : plate->get_obj_and_inst_set())
                objs.push_back(json::array({oi.first, oi.second}));
            jp["object_instances"] = objs;
            jplates.push_back(jp);
        }
        j["plates"] = jplates;

        json jobjs = json::array();
        for (size_t oi = 0; oi < model.objects.size(); ++oi) {
            const ModelObject *mo = model.objects[oi];
            json               jo;
            jo["index"] = oi;
            jo["name"]  = mo->name;
            json insts  = json::array();
            for (size_t ii = 0; ii < mo->instances.size(); ++ii) {
                const ModelInstance *inst = mo->instances[ii];
                const BoundingBoxf3  bb   = mo->instance_bounding_box(ii);
                const int            pi   = plate_of_instance(int(oi), int(ii));
                const Vec2d          o    = plate_origin(pi);
                json                 ji;
                ji["index"]                = ii;
                ji["plate"]                = pi;
                ji["center_on_plate_mm"]   = xy(Vec2d(bb.center().x() - o.x(), bb.center().y() - o.y()));
                ji["bbox_min_on_plate_mm"] = xyz(Vec3d(bb.min.x() - o.x(), bb.min.y() - o.y(), bb.min.z()));
                ji["bbox_max_on_plate_mm"] = xyz(Vec3d(bb.max.x() - o.x(), bb.max.y() - o.y(), bb.max.z()));
                ji["size_mm"]              = xyz(bb.size());
                const Vec3d rot            = inst->get_rotation() * (180. / M_PI);
                ji["rotation_deg"]         = xyz(rot);
                ji["scale"]                = xyz(inst->get_scaling_factor());
                ji["printable"]            = inst->printable;
                insts.push_back(ji);
            }
            jo["instances"] = insts;
            json   parts    = json::array();
            size_t tris     = 0;
            bool   enf = false, blk = false, land = false, color = false;
            for (const ModelVolume *mv : mo->volumes) {
                const char *type = mv->is_model_part() ? "part" :
                                   mv->is_negative_volume() ? "negative" :
                                   mv->is_modifier() ? "modifier" :
                                   mv->is_support_enforcer() ? "support_enforcer" :
                                   mv->is_support_blocker() ? "support_blocker" : "other";
                parts.push_back({{"name", mv->name}, {"type", type}});
                tris += mv->mesh().its.indices.size();
                if (mv->is_model_part() && !mv->supported_facets.empty()) {
                    enf  = enf || mv->supported_facets.has_facets(*mv, EnforcerBlockerType(1));
                    blk  = blk || mv->supported_facets.has_facets(*mv, EnforcerBlockerType(2));
                    land = land || mv->supported_facets.has_facets(*mv, SUPPORT_LANDING_STATE);
                }
                color = color || !mv->mmu_segmentation_facets.empty();
            }
            jo["volumes"]   = parts;
            jo["triangles"] = tris;
            jo["paint"]     = {{"support_enforce", enf}, {"support_block", blk}, {"supports_stand_here", land}, {"color", color}};
            json overrides  = json::object();
            for (const std::string &key : mo->config.keys())
                overrides[key] = mo->config.opt_serialize(key);
            jo["setting_overrides"] = overrides;
            jobjs.push_back(jo);
        }
        j["objects"] = jobjs;
        j["coordinates"] = "mm; x/y on a plate are measured from that plate's front left corner (its origin), z is the height above the plate";
        return j;
    });
}

// the 3D view (or the sliced preview) as a PNG image
json tool_render_view(const json &args)
{
    const std::string view   = lower(str(args, "view", "iso"));
    const std::string screen = lower(str(args, "screen", "keep"));
    const std::string fit    = lower(str(args, "fit", "plate"));
    const int         max_w  = std::clamp(int(num(args, "max_width", 1280)), 256, 2560);
    static const std::vector<std::string> views{"iso", "top", "bottom", "front", "rear", "left", "right", "keep"};
    if (std::find(views.begin(), views.end(), view) == views.end())
        throw ToolError("view must be one of iso, top, bottom, front, rear, left, right, keep");

    on_gui([=]() -> json {
        Plater *p = plater();
        // the 3D tab must be on screen (MiniMe may be showing Home or another tab)
        if (MainFrame *frame = wxGetApp().mainframe)
            frame->select_tab(size_t(screen == "preview" || (screen == "keep" && p->is_preview_shown()) ? MainFrame::tpPreview : MainFrame::tp3DEditor));
        if (screen == "preview")
            p->select_view_3D("Preview");
        else if (screen == "3d")
            p->select_view_3D("3D");
        GLCanvas3D *canvas = p->get_current_canvas3D();
        if (canvas == nullptr)
            throw ToolError("no 3D view");
        if (view != "keep")
            p->select_view(view);
        if (fit == "plate")
            canvas->zoom_to_plate(-1);
        else if (fit == "objects") {
            if (p->is_preview_shown())
                canvas->zoom_to_gcode();
            else
                canvas->zoom_to_volumes();
        }
        canvas->set_as_dirty();
        return {};
    });
    // let the view settle (the preview loads its toolpaths)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    struct Frame
    {
        int                  w{0}, h{0};
        std::vector<uint8_t> rgba;
    };
    auto promise = std::make_shared<std::promise<Frame>>();
    auto frame_f = promise->get_future();
    on_gui([promise]() -> json {
        GLCanvas3D *canvas = plater()->get_current_canvas3D();
        if (canvas == nullptr)
            throw ToolError("no 3D view");
        canvas->grab_next_frame([promise](int w, int h, std::vector<uint8_t> &&rgba) {
            try {
                promise->set_value(Frame{w, h, std::move(rgba)});
            } catch (...) {
            }
        });
        canvas->set_as_dirty();
        canvas->render();
        return {};
    });
    if (frame_f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        on_gui([]() -> json {
            if (GLCanvas3D *canvas = plater()->get_current_canvas3D())
                canvas->grab_next_frame(nullptr);
            return {};
        });
        throw ToolError("MiniMe did not draw the view (is its window minimized or hidden?)");
    }
    Frame frame = frame_f.get();

    // shrink to max_width (box filter), flip to top-down, drop alpha
    const int f  = std::max(1, (frame.w + max_w - 1) / max_w);
    const int ow = frame.w / f, oh = frame.h / f;
    std::vector<uint8_t> rgb(size_t(ow) * size_t(oh) * 3);
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x)
            for (int c = 0; c < 3; ++c) {
                int sum = 0;
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx)
                        sum += frame.rgba[(size_t(y * f + dy) * frame.w + size_t(x * f + dx)) * 4 + c];
                rgb[(size_t(oh - 1 - y) * ow + x) * 3 + c] = uint8_t(sum / (f * f));
            }
    const boost::filesystem::path path = boost::filesystem::path(data_dir()) / "log" / "claude_view.png";
    if (!png::write_rgb_to_file(path.string(), size_t(ow), size_t(oh), rgb))
        throw ToolError("could not write the image");
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    std::string             bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    const json state = on_gui([]() -> json {
        Plater *p = plater();
        return {{"screen", p->is_preview_shown() ? "preview" : "3d"},
                {"sliced", p->get_partplate_list().get_curr_plate()->is_slice_result_valid()}};
    });
    std::string note = "MiniMe " + state["screen"].get<std::string>() + " view, camera '" + view + "', " + std::to_string(ow) + "x" + std::to_string(oh) + " px.";
    if (state["screen"] == "preview" && !state["sliced"].get<bool>())
        note += " The plate is not sliced, so the preview shows no toolpaths: call slice first.";
    return {{"content", json::array({{{"type", "image"}, {"data", base64(bytes)}, {"mimeType", "image/png"}}, {{"type", "text"}, {"text", note}}})}};
}

json support_report_json()
{
    json report = json::object();
    for (const auto &[name, text] : support_report_get())
        report[name] = text;
    return report;
}

json tool_slice(const json &args)
{
    const int timeout_s = std::clamp(int(num(args, "timeout_seconds", 600)), 10, 3600);
    if (args.contains("plate") && !args["plate"].is_null()) {
        on_gui([args]() -> json {
            const int idx = plate_arg(args);
            if (idx != plater()->get_partplate_list().get_curr_plate_index())
                plater()->select_plate(idx);
            return {};
        });
    }
    on_gui([]() -> json {
        Plater    *p     = plater();
        PartPlate *plate = p->get_partplate_list().get_curr_plate();
        if (plate->get_obj_and_inst_set().empty())
            throw ToolError("the current plate is empty");
        close_gizmo();
        wxPostEvent(p, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
        return {};
    });

    const auto start = std::chrono::steady_clock::now();
    bool       seen_busy = false;
    int        retries   = 0;
    double     idle_since = -1.; // MiniMe looks idle for a moment between slicing and making the G-code
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const json st = on_gui([]() -> json {
            Plater *p = plater();
            return {{"slicing", p->is_background_process_slicing()},
                    {"scheduled", p->is_background_process_update_scheduled()},
                    {"valid", p->get_partplate_list().get_curr_plate()->is_slice_result_valid()}};
        });
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const bool   busy    = st["slicing"].get<bool>() || st["scheduled"].get<bool>();
        seen_busy            = seen_busy || busy;
        // a result that is still marked valid just after the request may be the old one: wait until MiniMe has picked the
        // request up (it gets busy), or long enough to be sure there was nothing to do
        if (st["valid"].get<bool>() && !busy && (seen_busy || elapsed > 3.))
            break;
        if (busy || st["valid"].get<bool>())
            idle_since = -1.;
        else if (idle_since < 0.)
            idle_since = elapsed;
        // an update still queued from earlier changes can mark the fresh result outdated right after it is made, and MiniMe
        // does not slice again by itself: ask again
        if (idle_since >= 0. && elapsed - idle_since > 1.5 && retries < 3) {
            ++retries;
            idle_since = -1.;
            on_gui([]() -> json {
                wxPostEvent(plater(), SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                return {};
            });
            continue;
        }
        if (idle_since >= 0. && elapsed - idle_since > 5. && (seen_busy || elapsed > 8.))
            throw ToolError("slicing stopped without a result - MiniMe shows the reason in its notifications (render_view can show it)");
        if (elapsed > timeout_s)
            throw ToolError("slicing takes longer than " + std::to_string(timeout_s) + " s; it goes on in MiniMe, call slice again later");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    return on_gui([seconds]() -> json {
        Plater    *p     = plater();
        PartPlate *plate = p->get_partplate_list().get_curr_plate();
        json       j;
        j["plate"]          = p->get_partplate_list().get_curr_plate_index();
        j["slice_seconds"]  = r2(seconds);
        if (GCodeProcessorResult *res = plate->get_slice_result()) {
            const auto &mode = res->print_statistics.modes[0];
            j["print_time"]  = format_time(mode.time);
            double support_s = 0.;
            for (const auto &rt : mode.roles_times)
                if (rt.first == erSupportMaterial || rt.first == erSupportMaterialInterface || rt.first == erSupportTransition)
                    support_s += rt.second;
            j["support_print_time"] = format_time(support_s);
            json warnings = json::array();
            for (const auto &w : res->warnings)
                warnings.push_back({{"level", w.level == 2 ? "error" : w.level == 1 ? "warning" : "tip"}, {"message", w.msg}, {"code", w.error_code}});
            j["warnings"] = warnings;
        }
        if (Print *print = plate->fff_print()) {
            const PrintStatistics &ps = print->print_statistics();
            j["filament_g"]           = r2(ps.total_weight);
            j["filament_m"]           = r2(ps.total_used_filament / 1000.);
            json layers               = json::object();
            for (const PrintObject *po : print->objects())
                layers[po->model_object()->name] = po->layer_count();
            j["layers_per_object"] = layers;
        }
        j["support_report"] = support_report_json();
        return j;
    });
}

json tool_support_report(const json &)
{
    return {{"support_report", support_report_json()},
            {"note", "one line per object from its last organic tree support generation; objects without tree supports are not listed"}};
}

json tool_find_settings(const json &args)
{
    const std::string query = lower(str(args, "query", ""));
    if (query.empty())
        throw ToolError("give a word to look for, e.g. 'support', 'brim', 'infill'");
    const int limit = std::clamp(int(num(args, "limit", 40)), 1, 200);
    return on_gui([query, limit]() -> json {
        PresetBundle            *pb    = wxGetApp().preset_bundle;
        const DynamicPrintConfig &proc  = pb->prints.get_edited_preset().config;
        const DynamicPrintConfig  full  = pb->full_config();
        json                      out   = json::array();
        for (const auto &[key, def] : print_config_def.options) {
            if (!full.has(key))
                continue;
            const std::string text = lower(key + " " + def.label + " " + def.full_label + " " + def.category);
            if (text.find(query) == std::string::npos)
                continue;
            out.push_back({{"key", key}, {"label", def.label}, {"category", def.category}, {"value", full.opt_serialize(key)},
                           {"changeable", proc.has(key) ? "yes (process setting)" : "no (printer or filament setting)"}});
            if (int(out.size()) >= limit)
                break;
        }
        return json{{"settings", out}};
    });
}

json tool_get_settings(const json &args)
{
    if (!args.contains("keys") || !args["keys"].is_array() || args["keys"].empty())
        throw ToolError("give 'keys', a list of setting keys (find_settings finds them)");
    return on_gui([args]() -> json {
        PresetBundle            *pb   = wxGetApp().preset_bundle;
        const DynamicPrintConfig full = pb->full_config();
        const ModelObject       *mo   = nullptr;
        if (args.contains("object") && !args["object"].is_null())
            mo = plater()->model().objects[find_object(args)];
        json out = json::object();
        for (const json &k : args["keys"]) {
            const std::string     key = k.is_string() ? k.get<std::string>() : "";
            const ConfigOptionDef *def = print_config_def.get(key);
            if (def == nullptr || !full.has(key)) {
                out[key] = {{"error", "unknown setting"}};
                continue;
            }
            json e;
            e["label"]   = def->label;
            e["tooltip"] = def->tooltip;
            e["value"]   = full.opt_serialize(key);
            e["source"]  = pb->prints.get_edited_preset().config.has(key) ? "process" : "printer or filament";
            if (mo != nullptr && mo->config.has(key)) {
                e["value"]  = mo->config.opt_serialize(key);
                e["source"] = "object override";
            }
            if (!def->enum_values.empty())
                e["choices"] = def->enum_values;
            if (def->min > -FLT_MAX && def->min < FLT_MAX)
                e["min"] = def->min;
            if (def->max > -FLT_MAX && def->max < FLT_MAX)
                e["max"] = def->max;
            if (!def->sidetext.empty())
                e["unit"] = def->sidetext;
            out[key] = e;
        }
        return json{{"settings", out}};
    });
}

json tool_set_settings(const json &args)
{
    if (!args.contains("values") || !args["values"].is_object() || args["values"].empty())
        throw ToolError("give 'values', e.g. {\"enable_support\": true, \"support_type\": \"tree(auto)\"}");
    return on_gui([args]() -> json {
        Plater                  *p    = plater();
        PresetBundle            *pb   = wxGetApp().preset_bundle;
        const DynamicPrintConfig &proc = pb->prints.get_edited_preset().config;
        const bool               for_object = args.contains("object") && !args["object"].is_null();
        // check every value first: nothing changes when one is wrong
        DynamicPrintConfig       changes;
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
        json                     before = json::object();
        for (const auto &[key, value] : args["values"].items()) {
            if (print_config_def.get(key) == nullptr)
                throw ToolError("unknown setting '" + key + "' (find_settings finds the right key)");
            if (!proc.has(key))
                throw ToolError("'" + key + "' is a printer or filament setting; only process settings can be changed here");
            try {
                changes.set_deserialize(key, value_text(value), ctx);
            } catch (const std::exception &e) {
                std::string choices;
                for (const std::string &c : print_config_def.get(key)->enum_values)
                    choices += (choices.empty() ? "; choices: " : ", ") + c;
                throw ToolError("bad value for '" + key + "': " + e.what() + choices);
            }
        }
        json result;
        if (for_object) {
            const int    obj_idx = find_object(args);
            ModelObject *mo      = p->model().objects[obj_idx];
            close_gizmo();
            Plater::TakeSnapshot snapshot(p, "Claude: settings of " + mo->name);
            for (const std::string &key : changes.keys()) {
                before[key] = mo->config.has(key) ? mo->config.opt_serialize(key) : proc.opt_serialize(key) + " (from the process)";
                mo->config.set_key_value(key, changes.option(key)->clone());
            }
            wxGetApp().obj_list()->object_config_options_changed(ObjectVolumeID{mo, nullptr});
            if (wxDataViewItem item = wxGetApp().obj_list()->GetModel()->GetItemById(obj_idx); item.IsOk())
                wxGetApp().obj_list()->add_settings_item(item, &mo->config.get());
            p->changed_object(obj_idx);
            p->set_plater_dirty(true);
            result["changed"] = "object '" + mo->name + "' (undo with undo)";
        } else {
            DynamicPrintConfig cfg = proc;
            for (const std::string &key : changes.keys()) {
                before[key] = proc.opt_serialize(key);
                cfg.set_key_value(key, changes.option(key)->clone());
            }
            Tab *tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
            if (tab == nullptr)
                throw ToolError("the process settings are not available");
            tab->load_config(cfg);
            result["changed"] = "the process preset '" + pb->prints.get_edited_preset().name + "' (not saved; set the old values back to revert)";
        }
        result["old_values"] = before;
        json now             = json::object();
        const DynamicPrintConfig full = pb->full_config();
        for (const std::string &key : changes.keys())
            now[key] = for_object ? p->model().objects[find_object(args)]->config.opt_serialize(key) : full.opt_serialize(key);
        result["new_values"] = now;
        return result;
    });
}

json tool_transform_object(const json &args)
{
    return on_gui([args]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const int    inst_idx = int(num(args, "instance", 0));
        if (inst_idx < 0 || inst_idx >= int(mo->instances.size()))
            throw ToolError("the object has " + std::to_string(mo->instances.size()) + " instance(s)");
        ModelInstance *inst = mo->instances[inst_idx];
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: move " + mo->name);

        if (args.contains("rotation_deg") && args["rotation_deg"].is_array() && args["rotation_deg"].size() == 3) {
            const json &r = args["rotation_deg"];
            inst->set_rotation(Vec3d(r[0].get<double>(), r[1].get<double>(), r[2].get<double>()) * (M_PI / 180.));
        }
        if (args.contains("rotate_z_by_deg")) {
            Vec3d rot = inst->get_rotation();
            rot.z() += num(args, "rotate_z_by_deg", 0) * (M_PI / 180.);
            inst->set_rotation(rot);
        }
        if (args.contains("scale")) {
            const json &s = args["scale"];
            if (s.is_number())
                inst->set_scaling_factor(Vec3d::Constant(s.get<double>()));
            else if (s.is_array() && s.size() == 3)
                inst->set_scaling_factor(Vec3d(s[0].get<double>(), s[1].get<double>(), s[2].get<double>()));
            else
                throw ToolError("'scale' is a factor (1 = as it is) or [x, y, z]");
            if (inst->get_scaling_factor().minCoeff() <= 0.)
                throw ToolError("scale must be above 0");
        }
        if (args.contains("center_on_plate_mm")) {
            const json &c = args["center_on_plate_mm"];
            if (!c.is_array() || c.size() != 2)
                throw ToolError("'center_on_plate_mm' is [x, y] on the plate");
            int pi = args.contains("plate") ? plate_arg(args) : plate_of_instance(obj_idx, inst_idx);
            const Vec2d       o  = plate_origin(pi);
            const BoundingBoxf3 bb = mo->instance_bounding_box(inst_idx);
            Vec3d off = inst->get_offset();
            off.x() += o.x() + c[0].get<double>() - bb.center().x();
            off.y() += o.y() + c[1].get<double>() - bb.center().y();
            inst->set_offset(off);
        }
        if (args.contains("move_by_mm")) {
            const json &m = args["move_by_mm"];
            if (!m.is_array() || m.size() < 2)
                throw ToolError("'move_by_mm' is [x, y] or [x, y, z]");
            inst->set_offset(inst->get_offset() + Vec3d(m[0].get<double>(), m[1].get<double>(), m.size() > 2 ? m[2].get<double>() : 0.));
        }
        mo->invalidate_bounding_box();
        // standing on the plate (MiniMe's own update lets objects sink into it)
        mo->ensure_on_bed(false);
        p->changed_object(obj_idx);
        p->get_partplate_list().notify_instance_update(obj_idx, inst_idx);
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        p->set_plater_dirty(true);

        const BoundingBoxf3 bb = mo->instance_bounding_box(inst_idx);
        const int           pi = plate_of_instance(obj_idx, inst_idx);
        const Vec2d         o  = plate_origin(pi);
        return {{"object", mo->name}, {"plate", pi}, {"center_on_plate_mm", xy(Vec2d(bb.center().x() - o.x(), bb.center().y() - o.y()))},
                {"size_mm", xyz(bb.size())}, {"rotation_deg", xyz(inst->get_rotation() * (180. / M_PI))}, {"scale", xyz(inst->get_scaling_factor())}};
    });
}

json tool_paint_supports(const json &args)
{
    const std::string paint = lower(str(args, "paint", ""));
    EnforcerBlockerType state;
    if (paint == "enforce")
        state = EnforcerBlockerType(1);
    else if (paint == "block")
        state = EnforcerBlockerType(2);
    else if (paint == "stand_here")
        state = SUPPORT_LANDING_STATE;
    else if (paint == "erase")
        state = EnforcerBlockerType::NONE;
    else
        throw ToolError("'paint' is enforce, block, stand_here or erase");
    const std::string facing = lower(str(args, "facing", "any"));
    if (facing != "up" && facing != "down" && facing != "side" && facing != "any")
        throw ToolError("'facing' is up, down, side or any");
    const double within = num(args, "within_deg", facing == "up" ? 15. : facing == "down" ? 50. : 15.);
    const double z_min  = num(args, "z_min_mm", -1e9), z_max = num(args, "z_max_mm", 1e9);
    std::optional<std::array<double, 4>> box;
    if (args.contains("box_on_plate_mm")) {
        const json &b = args["box_on_plate_mm"];
        if (!b.is_array() || b.size() != 4)
            throw ToolError("'box_on_plate_mm' is [x_min, y_min, x_max, y_max]");
        box = std::array<double, 4>{b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
    }
    const std::string clear = lower(str(args, "first_clear", "none"));

    return on_gui([=]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const int    inst_idx = std::clamp(int(num(args, "instance", 0)), 0, int(mo->instances.size()) - 1);
        const Vec2d  o        = plate_origin(plate_of_instance(obj_idx, inst_idx));
        const double cos_within = std::cos(within * M_PI / 180.), sin_within = std::sin(within * M_PI / 180.);
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: paint supports on " + mo->name);

        size_t facets = 0;
        double area   = 0.;
        for (ModelVolume *mv : mo->volumes) {
            if (!mv->is_model_part())
                continue;
            const indexed_triangle_set &its = mv->mesh().its;
            TriangleSelector            selector(mv->mesh());
            selector.deserialize(mv->supported_facets.get_data(), true);
            auto drop = [&](EnforcerBlockerType s) {
                // remove one paint everywhere, keeping the others
                selector.deserialize(selector.serialize(), true, EnforcerBlockerType::ExtruderMax, s, EnforcerBlockerType::NONE);
                selector.shift_states_above(s, 1);
            };
            if (clear == "same" && state != EnforcerBlockerType::NONE)
                drop(state);
            else if (clear == "all")
                selector.reset();
            const Transform3d trafo  = mo->instances[inst_idx]->get_matrix() * mv->get_matrix();
            const Matrix3d    normal = trafo.linear().inverse().transpose();
            for (int f = 0; f < int(its.indices.size()); ++f) {
                const Vec3d a = trafo * its.vertices[its.indices[f](0)].cast<double>();
                const Vec3d b = trafo * its.vertices[its.indices[f](1)].cast<double>();
                const Vec3d c = trafo * its.vertices[its.indices[f](2)].cast<double>();
                Vec3d       n = normal * (its.vertices[its.indices[f](1)] - its.vertices[its.indices[f](0)])
                                       .cross(its.vertices[its.indices[f](2)] - its.vertices[its.indices[f](0)]).cast<double>();
                const double len = n.norm();
                if (len <= 0.)
                    continue;
                n /= len;
                if ((facing == "up" && n.z() < cos_within) || (facing == "down" && -n.z() < cos_within) ||
                    (facing == "side" && std::abs(n.z()) > sin_within))
                    continue;
                const Vec3d center = (a + b + c) / 3.;
                if (center.z() < z_min || center.z() > z_max)
                    continue;
                if (box) {
                    const double x = center.x() - o.x(), y = center.y() - o.y();
                    if (x < (*box)[0] || x > (*box)[2] || y < (*box)[1] || y > (*box)[3])
                        continue;
                }
                selector.set_facet(f, state);
                ++facets;
                area += 0.5 * (b - a).cross(c - a).norm();
            }
            mv->supported_facets.set(selector);
        }
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        p->set_plater_dirty(true);
        p->schedule_background_process();
        return {{"object", mo->name}, {"painted", paint}, {"triangles", facets}, {"area_mm2", r2(area)},
                {"note", facets == 0 ? "nothing matched: loosen facing/within_deg/z/box" : "slice to see the effect; undo reverts it"}};
    });
}

json tool_support_zone(const json &args)
{
    const std::string zone   = lower(str(args, "zone", ""));
    const std::string action = lower(str(args, "action", "add"));
    int               zi     = zone == "no_supports" ? 0 : zone == "supports_only_here" ? 1 : -1;
    if (zi < 0)
        throw ToolError("'zone' is no_supports (red) or supports_only_here (green)");
    if (action != "add" && action != "remove" && action != "clear")
        throw ToolError("'action' is add, remove or clear");
    return on_gui([=]() -> json {
        Plater    *p        = plater();
        const int  pi       = plate_arg(args);
        PartPlate *plate    = p->get_partplate_list().get_plate(pi);
        ExPolygons current  = zone_areas(plate->config()->option<ConfigOptionPointsGroups>(ZONE_KEYS[zi]));
        Polygons   shape;
        if (action != "clear") {
            if (args.contains("rect_mm")) {
                const json &r = args["rect_mm"];
                if (!r.is_array() || r.size() != 4)
                    throw ToolError("'rect_mm' is [x_min, y_min, x_max, y_max] on the plate");
                const BoundingBox bb(Point(coord_t(scale_(r[0].get<double>())), coord_t(scale_(r[1].get<double>()))),
                                     Point(coord_t(scale_(r[2].get<double>())), coord_t(scale_(r[3].get<double>()))));
                shape.emplace_back(bb.polygon());
            } else if (args.contains("circle_mm")) {
                const json &c = args["circle_mm"];
                if (!c.is_array() || c.size() != 3)
                    throw ToolError("'circle_mm' is [x, y, radius] on the plate");
                Polygon      poly;
                const double r = c[2].get<double>();
                for (int i = 0; i < 64; ++i) {
                    const double a = 2. * M_PI * i / 64.;
                    poly.points.emplace_back(coord_t(scale_(c[0].get<double>() + r * std::cos(a))), coord_t(scale_(c[1].get<double>() + r * std::sin(a))));
                }
                shape.emplace_back(std::move(poly));
            } else if (args.contains("under_object")) {
                json a{{"object", args["under_object"]}};
                const int          obj_idx = find_object(a);
                const ModelObject *mo      = p->model().objects[obj_idx];
                const double       margin  = num(args, "margin_mm", 2.);
                const Vec2d        o       = plate_origin(pi);
                for (size_t ii = 0; ii < mo->instances.size(); ++ii) {
                    if (plate_of_instance(obj_idx, int(ii)) != pi)
                        continue;
                    // the outline of the object seen from above
                    Polygons outline;
                    for (const ModelVolume *mv : mo->volumes) {
                        if (!mv->is_model_part())
                            continue;
                        const Transform3d t = mo->instances[ii]->get_matrix() * mv->get_matrix();
                        const indexed_triangle_set &its = mv->mesh().its;
                        for (const auto &f : its.indices) {
                            Polygon tri;
                            for (int k = 0; k < 3; ++k) {
                                const Vec3d v = t * its.vertices[f(k)].cast<double>();
                                tri.points.emplace_back(coord_t(scale_(v.x() - o.x())), coord_t(scale_(v.y() - o.y())));
                            }
                            const double a = tri.area();
                            if (a < 0.)
                                tri.reverse();
                            if (a != 0.)
                                outline.emplace_back(std::move(tri));
                        }
                    }
                    append(shape, offset(union_(outline), scale_(margin)));
                }
                if (shape.empty())
                    throw ToolError("that object is not on plate " + std::to_string(pi));
            } else
                throw ToolError("give the shape: rect_mm, circle_mm or under_object");
        }
        ExPolygons result;
        if (action == "add") {
            Polygons all = to_polygons(current);
            append(all, shape);
            result = union_ex(all);
        } else if (action == "remove")
            result = diff_ex(to_polygons(current), shape);
        close_gizmo();
        {
            Plater::TakeSnapshot snapshot(p, "Claude: support zones on the plate", UndoRedo::SnapshotType::GizmoAction);
            std::vector<Vec2ds> values;
            for (const ExPolygon &ex : result) {
                Vec2ds pts;
                for (const Point &pt : ex.contour.points)
                    pts.emplace_back(unscale<double>(pt.x()), unscale<double>(pt.y()));
                values.emplace_back(std::move(pts));
                for (const Polygon &hole : ex.holes) {
                    Vec2ds hp;
                    for (const Point &pt : hole.points)
                        hp.emplace_back(unscale<double>(pt.x()), unscale<double>(pt.y()));
                    values.emplace_back(std::move(hp));
                }
            }
            if (values.empty())
                plate->config()->erase(ZONE_KEYS[zi]);
            else
                plate->config()->set_key_value(ZONE_KEYS[zi], new ConfigOptionPointsGroups(values));
        }
        p->set_plater_dirty(true);
        p->schedule_background_process();
        return {{"plate", pi}, {"zone", zone}, {"areas", result.size()}, {"total_mm2", r2(area_mm2(result))}};
    });
}

json tool_cut_object(const json &args)
{
    const std::string keep = lower(str(args, "keep", "both"));
    if (keep != "both" && keep != "upper" && keep != "lower")
        throw ToolError("'keep' is both, upper or lower");
    if (!args.contains("height_mm"))
        throw ToolError("give 'height_mm', the height of the cut above the plate");
    const double z    = num(args, "height_mm", 0.);
    const bool   flip = flag(args, "flip_upper", false);
    return on_gui([=]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const int    inst_idx = std::clamp(int(num(args, "instance", 0)), 0, int(mo->instances.size()) - 1);
        const BoundingBoxf3 bb = mo->instance_bounding_box(inst_idx);
        if (z <= bb.min.z() + 0.05 || z >= bb.max.z() - 0.05)
            throw ToolError("the object goes from " + std::to_string(r2(bb.min.z())) + " to " + std::to_string(r2(bb.max.z())) + " mm high; cut in between");
        if (!mo->cut_connectors.empty())
            throw ToolError("the object has cut connectors set up in the Cut tool; cut it there");
        const Vec3d  c = bb.center();
        const double l = std::max(bb.size().x(), bb.size().y()) + 10.;
        const std::array<Vec3d, 4> plane{Vec3d(c.x() - l, c.y() - l, z), Vec3d(c.x() + l, c.y() - l, z), Vec3d(c.x() + l, c.y() + l, z),
                                         Vec3d(c.x() - l, c.y() + l, z)};
        ModelObjectCutAttributes attributes = only_if(keep != "lower", ModelObjectCutAttribute::KeepUpper) |
                                              only_if(keep != "upper", ModelObjectCutAttribute::KeepLower) |
                                              only_if(flip, ModelObjectCutAttribute::FlipUpper) | only_if(true, ModelObjectCutAttribute::InvalidateCutInfo);
        close_gizmo();
        const std::string name = mo->name;
        Plater::TakeSnapshot snapshot(p, "Claude: cut " + name);
        p->cut(size_t(obj_idx), size_t(inst_idx), plane, attributes);
        p->set_plater_dirty(true);
        json made = json::array();
        Model &model = p->model();
        for (size_t i = 0; i < model.objects.size(); ++i)
            made.push_back({{"index", i}, {"name", model.objects[i]->name}});
        return {{"cut", name}, {"at_height_mm", r2(z)}, {"objects_now", made},
                {"note", "the pieces may overlap; move them apart with transform_object (or arrange in MiniMe)"}};
    });
}

json tool_add_model(const json &args)
{
    const std::string path = str(args, "path", "");
    if (path.empty())
        throw ToolError("give 'path', the full path of an .stl or .obj file");
    const boost::filesystem::path fp(path);
    const std::string ext = lower(fp.extension().string());
    if (ext != ".stl" && ext != ".obj")
        throw ToolError("only .stl and .obj files can be added this way (open projects in MiniMe yourself)");
    if (!boost::filesystem::exists(fp))
        throw ToolError("no such file: " + path);
    return on_gui([path]() -> json {
        Plater *p = plater();
        close_gizmo();
        if (MainFrame *frame = wxGetApp().mainframe)
            frame->select_tab(size_t(MainFrame::tp3DEditor));
        const size_t before = p->model().objects.size();
        p->load_files(std::vector<std::string>{path}, LoadStrategy::LoadModel);
        json added = json::array();
        for (size_t i = before; i < p->model().objects.size(); ++i)
            added.push_back({{"index", i}, {"name", p->model().objects[i]->name}});
        if (added.empty())
            throw ToolError("MiniMe did not load the file (see its notifications)");
        return {{"added", added}};
    }, 120000);
}

json tool_select_plate(const json &args)
{
    return on_gui([args]() -> json {
        const int idx = plate_arg(args);
        plater()->select_plate(idx);
        return {{"current_plate", idx}};
    });
}

json tool_undo(const json &args)
{
    const int  steps = std::clamp(int(num(args, "steps", 1)), 1, 50);
    const bool redo  = flag(args, "redo", false);
    return on_gui([=]() -> json {
        Plater *p = plater();
        close_gizmo();
        for (int i = 0; i < steps; ++i)
            redo ? p->redo() : p->undo();
        std::string top;
        p->undo_redo_topmost_string_getter(true, top);
        return {{"done", std::string(redo ? "redo " : "undo ") + std::to_string(steps)}, {"next_undo", top}};
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// the tool list

struct Tool
{
    std::string                       name;
    std::string                       description;
    json                              schema;
    std::function<json(const json &)> run;
};

const std::vector<Tool> &tools()
{
    static const std::string OBJ = R"J({"type": ["integer", "string"], "description": "the object: its index (0, 1, ...) or its name, as get_scene lists them"})J";
    static const std::vector<Tool> list = [] {
        std::vector<Tool> l;
        auto add = [&l](const char *name, const char *description, std::string schema, std::function<json(const json &)> run) {
            for (size_t pos; (pos = schema.find("$OBJ")) != std::string::npos;)
                schema.replace(pos, 4, OBJ);
            l.push_back(Tool{name, description, json::parse(schema), std::move(run)});
        };
        add("get_scene",
            "What is in MiniMe now: plates (with their support zones), objects with position, size, rotation, scale, paint and setting "
            "overrides, the printer / process / filament presets and the main support settings. Call this first.",
            R"J({"type": "object", "properties": {}})J", tool_get_scene);
        add("render_view", "A picture of MiniMe's 3D view or of the sliced preview (toolpaths, supports) as MiniMe draws it. Use it to check results.",
            R"J({"type": "object", "properties": {
                "view": {"type": "string", "enum": ["iso", "top", "bottom", "front", "rear", "left", "right", "keep"], "description": "camera direction; keep = as the user left it"},
                "screen": {"type": "string", "enum": ["3d", "preview", "keep"], "description": "3d = the models; preview = the sliced result"},
                "fit": {"type": "string", "enum": ["plate", "objects", "keep"]},
                "max_width": {"type": "integer", "description": "image width limit in pixels, default 1280"}}})J",
            tool_render_view);
        add("slice",
            "Slices the current plate (as the Slice button does), waits for it and returns print time, support print time, filament, "
            "warnings and the support report per object.",
            R"J({"type": "object", "properties": {
                "plate": {"type": "integer", "description": "plate to make current first"},
                "timeout_seconds": {"type": "integer"}}})J",
            tool_slice);
        add("support_report", "The last tree support summary per object: branches on the plate, on painted tops, in no-support zones, thinned trunks.",
            R"J({"type": "object", "properties": {}})J", tool_support_report);
        add("find_settings", "Finds setting keys whose key, label or category contains a word, with their current values.",
            R"J({"type": "object", "properties": {"query": {"type": "string"}, "limit": {"type": "integer"}}, "required": ["query"]})J", tool_find_settings);
        add("get_settings", "Values, meaning, choices and limits of settings; with 'object', that object's override wins.",
            R"J({"type": "object", "properties": {"keys": {"type": "array", "items": {"type": "string"}}, "object": $OBJ}, "required": ["keys"]})J",
            tool_get_settings);
        add("set_settings",
            "Changes process settings: for the whole project (the edited process preset, not saved), or with 'object' as overrides of "
            "that object only (one undo step). Values are checked first; nothing changes if one is wrong. Returns the old values.",
            R"J({"type": "object", "properties": {
                "values": {"type": "object", "description": "setting key -> value, e.g. {\"enable_support\": true, \"support_type\": \"tree(auto)\"}"},
                "object": $OBJ}, "required": ["values"]})J",
            tool_set_settings);
        add("transform_object", "Moves, turns or scales an object instance (one undo step). It is then dropped onto the plate.",
            R"J({"type": "object", "properties": {
                "object": $OBJ,
                "instance": {"type": "integer"},
                "center_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y]: put the middle of the object here"},
                "plate": {"type": "integer", "description": "with center_on_plate_mm: on this plate"},
                "move_by_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y] or [x, y, z]"},
                "rotation_deg": {"type": "array", "items": {"type": "number"}, "description": "absolute [x, y, z] rotation"},
                "rotate_z_by_deg": {"type": "number"},
                "scale": {"type": ["number", "array"], "description": "factor (1 = as it is) or [x, y, z]"}}, "required": ["object"]})J",
            tool_transform_object);
        add("paint_supports",
            "Paints support paint on an object's surface by a rule (one undo step): enforce = supports here, block = no supports here, "
            "stand_here = other objects' tree supports may stand on this surface (yellow), erase = remove support paint. "
            "The rule picks the triangles: which way they face, their height and an area of the plate.",
            R"J({"type": "object", "properties": {
                "object": $OBJ,
                "paint": {"type": "string", "enum": ["enforce", "block", "stand_here", "erase"]},
                "facing": {"type": "string", "enum": ["up", "down", "side", "any"], "description": "up = top surfaces, down = overhangs and bottoms"},
                "within_deg": {"type": "number", "description": "how far from straight up/down a surface may tilt (default 15 up, 50 down)"},
                "z_min_mm": {"type": "number"},
                "z_max_mm": {"type": "number"},
                "box_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x_min, y_min, x_max, y_max]"},
                "first_clear": {"type": "string", "enum": ["none", "same", "all"], "description": "same = remove this paint everywhere on the object first; all = remove all support paint first"},
                "instance": {"type": "integer", "description": "instance whose position the heights and box refer to"}}, "required": ["object", "paint"]})J",
            tool_paint_supports);
        add("support_zone",
            "Paints the plate (one undo step): no_supports = tree supports may not stand there (red); supports_only_here = they may stand "
            "only there (green). Add or remove a rectangle, a circle or the outline under an object, or clear the zone.",
            R"J({"type": "object", "properties": {
                "zone": {"type": "string", "enum": ["no_supports", "supports_only_here"]},
                "action": {"type": "string", "enum": ["add", "remove", "clear"]},
                "rect_mm": {"type": "array", "items": {"type": "number"}, "description": "[x_min, y_min, x_max, y_max] on the plate"},
                "circle_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y, radius] on the plate"},
                "under_object": $OBJ,
                "margin_mm": {"type": "number", "description": "with under_object: grow the outline by this much (default 2)"},
                "plate": {"type": "integer"}}, "required": ["zone"]})J",
            tool_support_zone);
        add("cut_object",
            "Cuts an object with a flat horizontal cut at a height (one undo step). For angled, curved or shaped cuts and connectors the "
            "user uses the Cut tool.",
            R"J({"type": "object", "properties": {
                "object": $OBJ,
                "height_mm": {"type": "number", "description": "height above the plate"},
                "keep": {"type": "string", "enum": ["both", "upper", "lower"]},
                "flip_upper": {"type": "boolean", "description": "turn the upper part over so it stands on the cut face"},
                "instance": {"type": "integer"}}, "required": ["object", "height_mm"]})J",
            tool_cut_object);
        add("add_model", "Adds the objects of an .stl or .obj file to the current plate (one undo step). It never replaces the project.",
            R"J({"type": "object", "properties": {"path": {"type": "string", "description": "full path of the file on this computer"}}, "required": ["path"]})J",
            tool_add_model);
        add("select_plate", "Makes a plate the current one.", R"J({"type": "object", "properties": {"plate": {"type": "integer"}}, "required": ["plate"]})J",
            tool_select_plate);
        add("undo", "Undoes (or with redo=true, redoes) the last steps in MiniMe, including the user's own.",
            R"J({"type": "object", "properties": {"steps": {"type": "integer"}, "redo": {"type": "boolean"}}})J", tool_undo);
        return l;
    }();
    return list;
}

const char *INSTRUCTIONS =
    "MiniMe Slicer (a Bambu Studio fork for FDM 3D printing) is open on the user's computer. Its tools let you look at the plate, slice, "
    "and change objects, supports and settings. Start with get_scene. Lengths are millimetres; x/y on a plate are measured from its "
    "front left corner, z is the height above the plate. Each change is one undo step in MiniMe. After changing supports or settings, "
    "slice and look at the result (render_view with screen=preview) before saying it worked. You cannot print or send to a printer.";

// ---------------------------------------------------------------------------------------------------------------------
// MCP (JSON-RPC 2.0 over HTTP POST, plain JSON answers)

json rpc_error(const json &id, int code, const std::string &message) { return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}; }

json handle_rpc(const json &msg)
{
    if (!msg.is_object() || !msg.contains("method"))
        return rpc_error(nullptr, -32600, "invalid request");
    const std::string method = msg["method"].get<std::string>();
    const bool        is_notification = !msg.contains("id");
    const json        id     = is_notification ? json() : msg["id"];
    const json        params = msg.contains("params") ? msg["params"] : json::object();
    if (is_notification)
        return json(); // notifications/initialized, cancelled... nothing to answer

    if (method == "initialize") {
        const std::string version = params.contains("protocolVersion") && params["protocolVersion"].is_string() ? params["protocolVersion"].get<std::string>() : "2025-06-18";
        return {{"jsonrpc", "2.0"},
                {"id", id},
                {"result",
                 {{"protocolVersion", version},
                  {"capabilities", {{"tools", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", "minime-slicer"}, {"version", SLIC3R_VERSION}}},
                  {"instructions", INSTRUCTIONS}}}};
    }
    if (method == "ping")
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    if (method == "tools/list") {
        json list = json::array();
        for (const Tool &t : tools())
            list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", list}}}};
    }
    if (method == "tools/call") {
        const std::string name = params.value("name", "");
        const json        args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
        for (const Tool &t : tools()) {
            if (name != t.name)
                continue;
            json result;
            try {
                std::lock_guard<std::mutex> lock(s_tool_mutex);
                BOOST_LOG_TRIVIAL(info) << "Claude bridge: " << name << " " << args.dump();
                json out = t.run(args);
                if (out.is_object() && out.contains("content"))
                    result = out;
                else
                    result = {{"content", json::array({{{"type", "text"}, {"text", out.dump(1)}}})}};
                result["isError"] = false;
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(info) << "Claude bridge: " << name << " failed: " << e.what();
                result = {{"content", json::array({{{"type", "text"}, {"text", e.what()}}})}, {"isError", true}};
            }
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
        }
        return rpc_error(id, -32602, "unknown tool '" + name + "'");
    }
    return rpc_error(id, -32601, "method not found: " + method);
}

bool local_name(const std::string &host_port)
{
    std::string host = host_port;
    if (!host.empty() && host.front() == '[')
        host = host.substr(0, host.find(']') + 1);
    else
        host = host.substr(0, host.find(':'));
    host = lower(host);
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

void serve(tcp::socket &socket)
{
#ifdef _WIN32
    DWORD timeout_ms = 10000;
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeout_ms), sizeof(timeout_ms));
#endif
    boost::beast::flat_buffer                buffer;
    http::request_parser<http::string_body>  parser;
    parser.body_limit(16 * 1024 * 1024);
    boost::beast::error_code ec;
    http::read(socket, buffer, parser, ec);
    if (ec)
        return;
    const http::request<http::string_body> &req = parser.get();
    http::response<http::string_body>        res{http::status::ok, req.version()};
    res.set(http::field::server, "minime-slicer");
    res.keep_alive(false);

    const std::string origin(req[http::field::origin]);
    const std::string host(req[http::field::host]);
    std::string       target(req.target());
    target = target.substr(0, target.find('?'));
    if (!local_name(host) || (!origin.empty() && !local_name(origin.substr(origin.find("://") == std::string::npos ? 0 : origin.find("://") + 3)))) {
        // only this computer: also refuses web pages trying to reach it (DNS rebinding)
        res.result(http::status::forbidden);
        res.body() = "forbidden";
    } else if (target != "/mcp" && target != "/") {
        res.result(http::status::not_found);
        res.body() = "MiniMe's Claude bridge is at /mcp";
    } else if (req.method() != http::verb::post) {
        res.result(http::status::method_not_allowed);
        res.set(http::field::allow, "POST");
    } else {
        json answer;
        try {
            const json msg = json::parse(req.body());
            if (msg.is_array()) {
                answer = json::array();
                for (const json &m : msg)
                    if (json a = handle_rpc(m); !a.is_null())
                        answer.push_back(a);
                if (answer.empty())
                    answer = json();
            } else
                answer = handle_rpc(msg);
        } catch (const std::exception &e) {
            answer = rpc_error(nullptr, -32700, std::string("parse error: ") + e.what());
        }
        if (answer.is_null())
            res.result(http::status::accepted);
        else {
            res.set(http::field::content_type, "application/json");
            res.body() = answer.dump();
        }
    }
    res.prepare_payload();
    http::write(socket, res, ec);
    socket.shutdown(tcp::socket::shutdown_both, ec);
}

void run_server(int first_port, int tries)
{
    boost::asio::io_context io;
    tcp::acceptor           acceptor(io);
    boost::system::error_code ec;
    int port = first_port;
    for (int i = 0; i < std::max(1, tries); ++i) {
        port = first_port + i;
        ec.clear();
        if (acceptor.is_open())
            acceptor.close(ec);
        const tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), (unsigned short) port);
        acceptor.open(ep.protocol(), ec);
#ifdef _WIN32
        if (!ec) {
            BOOL exclusive = TRUE;
            ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
        }
#endif
        if (!ec)
            acceptor.bind(ep, ec);
        if (!ec)
            acceptor.listen(8, ec);
        if (!ec)
            acceptor.non_blocking(true, ec);
        if (!ec)
            break;
    }
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "Claude bridge: cannot listen on 127.0.0.1:" << port << ": " << ec.message();
        set_status("cannot use port " + std::to_string(port) + " (" + ec.message() + "); another MiniMe may have it");
        s_running = false;
        return;
    }
    s_port = port;
    BOOST_LOG_TRIVIAL(info) << "Claude bridge: listening on http://127.0.0.1:" << port << "/mcp";
    set_status("on: http://127.0.0.1:" + std::to_string(port) + "/mcp");
    while (!s_stop) {
        tcp::socket socket(io);
        acceptor.accept(socket, ec);
        if (ec) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ec == boost::asio::error::would_block || ec == boost::asio::error::try_again ? 50 : 500));
            continue;
        }
        socket.non_blocking(false, ec);
        try {
            serve(socket);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "Claude bridge: " << e.what();
        }
    }
    acceptor.close(ec);
    s_port = 0;
    set_status("off");
    s_running = false;
}

} // namespace

void ClaudeBridge::start(int port, int tries)
{
    if (s_running)
        return;
    if (s_thread.joinable())
        s_thread.join();
    s_stop    = false;
    s_running = true;
    set_status("starting");
    s_thread = std::thread([port, tries] { run_server(port, tries); });
}

void ClaudeBridge::stop()
{
    s_stop = true;
    if (s_thread.joinable())
        s_thread.join();
    s_running = false;
    set_status("off");
}

bool ClaudeBridge::running() { return s_running; }

int ClaudeBridge::port() { return s_port; }

void ClaudeBridge::set_needed_by_chat(bool needed)
{
    s_chat_needs = needed;
    apply_preference();
}

std::string ClaudeBridge::status()
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    return s_status;
}

void ClaudeBridge::apply_preference()
{
    AppConfig *config = wxGetApp().app_config;
    if (s_chat_needs)
        // the chat finds a free port itself; an external Claude uses the default one
        start(DEFAULT_PORT, 10);
    else if (config != nullptr && config->get_bool(CONFIG_KEY))
        start();
    else
        stop();
}

} // namespace Slic3r::GUI
