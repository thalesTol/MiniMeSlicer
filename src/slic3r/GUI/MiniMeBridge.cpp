#include "MiniMeBridge.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_ObjectList.hpp"
#include "MiniMePricing.hpp"
#include "ObjectDataViewModel.hpp"
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
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"

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
// parts, filaments and layers: exact data and edits, so Claude does not have to guess from pictures

int filament_count() { return int(wxGetApp().preset_bundle->filament_presets.size()); }

int check_filament(int f, bool allow_zero)
{
    const int n = filament_count();
    if (f < (allow_zero ? 0 : 1) || f > n)
        throw ToolError("filament must be " + std::string(allow_zero ? "0 (the object's own) or " : "") + "1.." + std::to_string(n) +
                        " (get_scene lists the filaments)");
    return f;
}

// the filaments of the project: number (1-based, as MiniMe shows them), preset, colour, type
json filaments_json()
{
    PresetBundle            *pb   = wxGetApp().preset_bundle;
    const DynamicPrintConfig full = pb->full_config();
    const auto *colours = full.option<ConfigOptionStrings>("filament_colour");
    const auto *types   = full.option<ConfigOptionStrings>("filament_type");
    json out = json::array();
    for (size_t i = 0; i < pb->filament_presets.size(); ++i)
        out.push_back({{"filament", i + 1},
                       {"preset", pb->filament_presets[i]},
                       {"colour", colours != nullptr && i < colours->values.size() ? colours->values[i] : ""},
                       {"type", types != nullptr && i < types->values.size() ? types->values[i] : ""}});
    return out;
}

int object_filament(const ModelObject *mo)
{
    const ConfigOption *opt = mo->config.option("extruder");
    return opt != nullptr && opt->getInt() > 0 ? opt->getInt() : 1;
}

// the filament a part prints with where it is not painted
int part_filament(const ModelObject *mo, const ModelVolume *mv)
{
    const ConfigOption *opt = mv->config.option("extruder");
    return opt != nullptr && opt->getInt() > 0 ? opt->getInt() : object_filament(mo);
}

const char *volume_type_name(const ModelVolume *mv)
{
    return mv->is_model_part() ? "part" :
           mv->is_negative_volume() ? "negative" :
           mv->is_modifier() ? "modifier" :
           mv->is_support_enforcer() ? "support_enforcer" :
           mv->is_support_blocker() ? "support_blocker" : "other";
}

// a part where it is on the plate (instance 0)
BoundingBoxf3 part_box(const ModelObject *mo, const ModelVolume *mv, int inst_idx = 0)
{
    return mv->mesh().transformed_bounding_box(mo->instances[inst_idx]->get_matrix() * mv->get_matrix());
}

// a part of an object by index or name
int find_part(const ModelObject *mo, const json &args, const char *key = "part")
{
    if (!args.contains(key) || args[key].is_null())
        throw ToolError("say which part (its index or name, as get_scene lists them under the object's parts)");
    const json &v = args[key];
    if (v.is_number_integer()) {
        const int idx = v.get<int>();
        if (idx < 0 || idx >= int(mo->volumes.size()))
            throw ToolError("the object has parts 0.." + std::to_string(int(mo->volumes.size()) - 1));
        return idx;
    }
    if (v.is_string()) {
        const std::string name = v.get<std::string>();
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (mo->volumes[i]->name == name)
                return int(i);
        int found = -1, count = 0;
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (lower(mo->volumes[i]->name).find(lower(name)) != std::string::npos) {
                found = int(i);
                ++count;
            }
        if (count == 1)
            return found;
        throw ToolError(count == 0 ? "the object has no part called '" + name + "'" : "several parts match '" + name + "', use the index");
    }
    throw ToolError("'part' must be an index or a name");
}

json box_on_plate(const BoundingBoxf3 &bb, int plate_idx)
{
    const Vec2d o = plate_origin(plate_idx);
    return {{"min", xyz(Vec3d(bb.min.x() - o.x(), bb.min.y() - o.y(), bb.min.z()))},
            {"max", xyz(Vec3d(bb.max.x() - o.x(), bb.max.y() - o.y(), bb.max.z()))},
            {"z_mm", json::array({r2(bb.min.z()), r2(bb.max.z())})}};
}

// which filaments are painted on a part, and between which heights
json painted_json(const ModelObject *mo, const ModelVolume *mv)
{
    json out = json::array();
    if (mv->mmu_segmentation_facets.empty())
        return out;
    const Transform3d trafo = mo->instances.front()->get_matrix() * mv->get_matrix();
    for (int f = 1; f <= filament_count(); ++f) {
        const indexed_triangle_set facets = mv->mmu_segmentation_facets.get_facets(*mv, EnforcerBlockerType(f));
        if (facets.indices.empty())
            continue;
        double zmin = DBL_MAX, zmax = -DBL_MAX;
        for (const Vec3f &v : facets.vertices) {
            const double z = (trafo * v.cast<double>()).z();
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
        out.push_back({{"filament", f}, {"triangles", facets.indices.size()}, {"z_mm", json::array({r2(zmin), r2(zmax)})}});
    }
    return out;
}

// the parts of an object with what Claude needs to plan: type, filament, where they are, paint, mesh errors
json parts_json(int obj_idx, const ModelObject *mo, int plate_idx)
{
    json parts = json::array();
    for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
        const ModelVolume *mv = mo->volumes[vi];
        json jp;
        jp["index"]  = vi;
        jp["name"]   = mv->name;
        jp["type"]   = volume_type_name(mv);
        if (mv->is_model_part() || mv->is_modifier()) {
            jp["filament"] = part_filament(mo, mv);
            if (!mv->config.has("extruder") || mv->config.option("extruder")->getInt() <= 0)
                jp["filament_from"] = "object";
        }
        jp["box_on_plate_mm"] = box_on_plate(part_box(mo, mv), plate_idx);
        jp["size_mm"]         = xyz(part_box(mo, mv).size());
        if (mv->is_model_part()) {
            const json painted = painted_json(mo, mv);
            if (!painted.empty())
                jp["painted_filaments"] = painted;
        }
        MeshIssueCounts issues;
        wxGetApp().obj_list()->get_mesh_errors_info(obj_idx, int(vi), nullptr, &issues);
        if (issues.open_edges > 0 || issues.non_manifold_edges > 0 || issues.non_manifold_vertices > 0 || issues.has_reversed_faces)
            jp["mesh_errors"] = {{"open_edges", issues.open_edges}, {"non_manifold_edges", issues.non_manifold_edges},
                                 {"non_manifold_vertices", issues.non_manifold_vertices}, {"reversed_faces", issues.has_reversed_faces}};
        json overrides = json::object();
        for (const std::string &key : mv->config.keys())
            if (key != "extruder")
                overrides[key] = mv->config.opt_serialize(key);
        if (!overrides.empty())
            jp["setting_overrides"] = overrides;
        parts.push_back(jp);
    }
    return parts;
}

json tool_layers(const json &args)
{
    const int max_rows = std::clamp(int(num(args, "max_rows", 80)), 5, 1000);
    return on_gui([max_rows]() -> json {
        Plater    *p     = plater();
        PartPlate *plate = p->get_partplate_list().get_curr_plate();
        Print     *print = plate->fff_print();
        if (print == nullptr || !plate->is_slice_result_valid())
            throw ToolError("the current plate is not sliced (or the slice is outdated): call slice first");
        const ToolOrdering &order = print->tool_ordering();
        struct Run
        {
            int                       first, last;
            double                    z0, z1;
            std::vector<unsigned int> filaments;
        };
        std::vector<Run>   runs;
        std::map<int, int> layers_of;
        json               custom = json::array();
        int                layer_no = 0, changes = 0, current = -1;
        for (const LayerTools &lt : order.layer_tools()) {
            if (lt.extruders.empty())
                continue;
            ++layer_no;
            std::vector<unsigned int> fil;
            for (unsigned int e : lt.extruders) {
                if (current >= 0 && int(e) != current)
                    ++changes;
                current = int(e);
                fil.push_back(e + 1);
                ++layers_of[int(e) + 1];
            }
            if (lt.custom_gcode != nullptr)
                custom.push_back({{"layer", layer_no}, {"z_mm", r2(lt.print_z)}, {"type", int(lt.custom_gcode->type)}, {"filament", lt.custom_gcode->extruder}});
            if (!runs.empty() && runs.back().filaments == fil && runs.back().last == layer_no - 1) {
                runs.back().last = layer_no;
                runs.back().z1   = lt.print_z;
            } else
                runs.push_back({layer_no, layer_no, lt.print_z, lt.print_z, fil});
        }
        json rows = json::array();
        for (const Run &r : runs) {
            if (int(rows.size()) >= max_rows) {
                rows.push_back({{"note", "more rows left out; call again with a higher max_rows"}});
                break;
            }
            rows.push_back({{"layers", r.first == r.last ? std::to_string(r.first) : std::to_string(r.first) + "-" + std::to_string(r.last)},
                            {"top_z_mm", r.first == r.last ? json(r2(r.z1)) : json::array({r2(r.z0), r2(r.z1)})},
                            {"filaments_in_print_order", r.filaments}});
        }
        json per = json::object();
        for (const auto &[f, n] : layers_of)
            per[std::to_string(f)] = n;
        json j;
        j["layers"]                   = layer_no;
        j["filament_changes"]         = changes;
        j["layers_per_filament"]      = per;
        j["layer_rows"]               = rows;
        if (!custom.empty())
            j["custom_gcodes"] = custom;
        j["note"] = "Each row is a run of layers with the same filaments in the same print order; every switch between two "
                    "filaments is one filament change (purge). Filament numbers are MiniMe's (1-based).";
        return j;
    });
}

json tool_set_filament(const json &args)
{
    if (!args.contains("filament"))
        throw ToolError("give 'filament', the filament number (get_scene lists them)");
    return on_gui([args]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const bool   is_part = args.contains("part") && !args["part"].is_null();
        const int    f       = check_filament(int(num(args, "filament", 1)), is_part);
        close_gizmo();
        json reset = json::array();
        std::vector<ObjectVolumeID> ids;
        if (is_part) {
            const int vol_idx = find_part(mo, args);
            ModelVolume *mv   = mo->volumes[vol_idx];
            if (!mv->is_model_part() && !mv->is_modifier())
                throw ToolError("only parts and modifiers have a filament");
            ids.push_back({mo, mv});
        } else {
            for (const ModelVolume *mv : mo->volumes)
                if (mv->is_model_part() && mv->config.has("extruder") && mv->config.option("extruder")->getInt() > 0 && mo->volumes.size() > 1)
                    reset.push_back(mv->name);
            ids.push_back({mo, nullptr});
        }
        ObjectList *list = wxGetApp().obj_list();
        list->select_items(ids);
        list->set_extruder_for_selected_items(f);
        p->set_plater_dirty(true);
        json j = {{"object", mo->name}, {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
        if (!reset.empty())
            j["note"] = "setting the object's filament also set these parts to it: " + reset.dump();
        return j;
    });
}

json tool_assemble(const json &args)
{
    if (!args.contains("objects") || !args["objects"].is_array() || args["objects"].size() < 2)
        throw ToolError("give 'objects', a list of at least two objects (index or name)");
    return on_gui([args]() -> json {
        Plater             *p = plater();
        std::vector<size_t> idxs;
        for (const json &o : args["objects"]) {
            const int idx = find_object(json{{"object", o}});
            if (std::find(idxs.begin(), idxs.end(), size_t(idx)) == idxs.end())
                idxs.push_back(size_t(idx));
        }
        if (idxs.size() < 2)
            throw ToolError("give at least two different objects");
        close_gizmo();
        const size_t before = p->model().objects.size();
        wxGetApp().obj_list()->merge_objects(idxs);
        Model &model = p->model();
        if (model.objects.size() != before - idxs.size() + 1)
            throw ToolError("MiniMe did not assemble them (are they on the same plate?)");
        // the assembled object is the one with the most parts
        int best = 0;
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (model.objects[i]->volumes.size() > model.objects[best]->volumes.size())
                best = int(i);
        const ModelObject *mo = model.objects[best];
        return {{"assembled_object", best}, {"name", mo->name}, {"parts", parts_json(best, mo, plate_of_instance(best, 0))},
                {"note", "the parts kept their places; parts can now sit at any height inside the object (move_part)"}};
    });
}

json tool_move_part(const json &args)
{
    return on_gui([args]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const int    vol_idx = find_part(mo, args);
        ModelVolume *mv      = mo->volumes[vol_idx];
        if (mo->volumes.size() < 2)
            throw ToolError("the object has a single part; move the object with transform_object");
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: move part " + mv->name);
        const ModelInstance *inst = mo->instances.front();
        const int            pi   = plate_of_instance(obj_idx, 0);
        if (args.contains("size_mm")) {
            const json &s = args["size_mm"];
            if (!s.is_array() || s.size() != 3)
                throw ToolError("'size_mm' is [x, y, z]; 0 keeps that size");
            const Vec3d now = part_box(mo, mv).size();
            Vec3d       f(1., 1., 1.);
            for (int a = 0; a < 3; ++a)
                if (s[a].get<double>() > 0. && now[a] > 1e-6)
                    f[a] = s[a].get<double>() / now[a];
            mv->set_scaling_factor(mv->get_scaling_factor().cwiseProduct(f));
        }
        const BoundingBoxf3 bb = part_box(mo, mv);
        Vec3d               d  = Vec3d::Zero();
        if (args.contains("bottom_z_mm"))
            d.z() = num(args, "bottom_z_mm", 0.) - bb.min.z();
        if (args.contains("center_on_plate_mm")) {
            const json &c = args["center_on_plate_mm"];
            if (!c.is_array() || c.size() != 2)
                throw ToolError("'center_on_plate_mm' is [x, y] on the plate");
            const Vec2d o = plate_origin(pi);
            d.x()         = o.x() + c[0].get<double>() - bb.center().x();
            d.y()         = o.y() + c[1].get<double>() - bb.center().y();
        }
        if (args.contains("move_by_mm")) {
            const json &m = args["move_by_mm"];
            if (!m.is_array() || m.size() < 2)
                throw ToolError("'move_by_mm' is [x, y] or [x, y, z]");
            d += Vec3d(m[0].get<double>(), m[1].get<double>(), m.size() > 2 ? m[2].get<double>() : 0.);
        }
        // from plate to object coordinates
        const Vec3d d_obj = inst->get_matrix(true).inverse() * d;
        mv->set_offset(mv->get_offset() + d_obj);
        mo->invalidate_bounding_box();
        mo->ensure_on_bed(false);
        p->changed_object(obj_idx);
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        p->set_plater_dirty(true);
        return {{"object", mo->name}, {"parts", parts_json(obj_idx, mo, pi)},
                {"note", "the object as a whole always stands on the plate: its lowest part is at z 0"}};
    });
}

json tool_delete(const json &args)
{
    return on_gui([args]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        close_gizmo();
        if (args.contains("parts") && args["parts"].is_array() && !args["parts"].empty()) {
            // several parts, highest index first so that the others keep theirs
            std::vector<int> idxs;
            for (const json &e : args["parts"]) {
                const int vi = find_part(mo, json{{"part", e}});
                if (std::find(idxs.begin(), idxs.end(), vi) == idxs.end())
                    idxs.push_back(vi);
            }
            std::sort(idxs.rbegin(), idxs.rend());
            int solid_left = 0;
            for (int vi = 0; vi < int(mo->volumes.size()); ++vi)
                if (mo->volumes[vi]->is_model_part() && std::find(idxs.begin(), idxs.end(), vi) == idxs.end())
                    ++solid_left;
            if (solid_left == 0)
                throw ToolError("that would delete every solid part; delete the object instead");
            Plater::TakeSnapshot snapshot(p, "Claude: delete parts of " + mo->name);
            json names = json::array();
            for (int vi : idxs) {
                names.push_back(mo->volumes[vi]->name);
                wxGetApp().obj_list()->delete_from_model_and_list(itVolume, obj_idx, vi);
            }
            p->set_plater_dirty(true);
            return {{"deleted_parts", names}, {"object", mo->name}, {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
        }
        if (args.contains("part") && !args["part"].is_null()) {
            const int vol_idx = find_part(mo, args);
            const int parts   = int(std::count_if(mo->volumes.begin(), mo->volumes.end(), [](const ModelVolume *v) { return v->is_model_part(); }));
            if (mo->volumes[vol_idx]->is_model_part() && parts < 2)
                throw ToolError("that is the object's only part; delete the object instead");
            const std::string name = mo->volumes[vol_idx]->name;
            wxGetApp().obj_list()->delete_from_model_and_list(itVolume, obj_idx, vol_idx);
            p->set_plater_dirty(true);
            return {{"deleted_part", name}, {"object", mo->name}, {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
        }
        const std::string name = mo->name;
        wxGetApp().obj_list()->delete_from_model_and_list(itObject, obj_idx, -1);
        p->set_plater_dirty(true);
        json left = json::array();
        for (size_t i = 0; i < p->model().objects.size(); ++i)
            left.push_back({{"index", i}, {"name", p->model().objects[i]->name}});
        return {{"deleted_object", name}, {"objects_now", left}};
    });
}

// a solid from outlines: bottom at z0, top at z1 (unscaled mm, plate world coordinates)
indexed_triangle_set extrude_outlines(const ExPolygons &outlines, double z0, double z1)
{
    indexed_triangle_set its;
    auto add_tri = [&its](const Vec3d &a, const Vec3d &b, const Vec3d &c) {
        const int i = int(its.vertices.size());
        its.vertices.emplace_back(a.cast<float>());
        its.vertices.emplace_back(b.cast<float>());
        its.vertices.emplace_back(c.cast<float>());
        its.indices.emplace_back(i, i + 1, i + 2);
    };
    const std::vector<Vec3d> top    = triangulate_expolygons_3d(outlines, z1, NORMALS_UP);
    const std::vector<Vec3d> bottom = triangulate_expolygons_3d(outlines, z0, !NORMALS_UP);
    for (size_t i = 0; i + 2 < top.size(); i += 3)
        add_tri(top[i], top[i + 1], top[i + 2]);
    for (size_t i = 0; i + 2 < bottom.size(); i += 3)
        add_tri(bottom[i], bottom[i + 1], bottom[i + 2]);
    auto walls = [&](const Polygon &poly) {
        const size_t n = poly.points.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2d a = unscale(poly.points[i]), b = unscale(poly.points[(i + 1) % n]);
            add_tri(Vec3d(a.x(), a.y(), z0), Vec3d(b.x(), b.y(), z0), Vec3d(b.x(), b.y(), z1));
            add_tri(Vec3d(a.x(), a.y(), z0), Vec3d(b.x(), b.y(), z1), Vec3d(a.x(), a.y(), z1));
        }
    };
    for (const ExPolygon &ex : outlines) {
        walls(ex.contour);
        for (const Polygon &hole : ex.holes)
            walls(hole);
    }
    its_merge_vertices(its);
    return its;
}

ExPolygon rounded_rect(double w, double h, double r, double cx, double cy)
{
    r = std::clamp(r, 0., std::min(w, h) / 2. - 0.01);
    Polygon     poly;
    const int   seg = r > 0. ? std::max(4, int(std::ceil(r * 2.))) : 0;
    const Vec2d corners[4] = {{cx + w / 2 - r, cy - h / 2 + r}, {cx + w / 2 - r, cy + h / 2 - r}, {cx - w / 2 + r, cy + h / 2 - r}, {cx - w / 2 + r, cy - h / 2 + r}};
    for (int c = 0; c < 4; ++c) {
        const double a0 = -M_PI / 2. + c * M_PI / 2.;
        if (seg == 0)
            poly.points.emplace_back(scaled(corners[c].x() + (c == 0 || c == 1 ? r : -r)), scaled(corners[c].y() + (c == 1 || c == 2 ? r : -r)));
        for (int i = 0; i <= seg && seg > 0; ++i) {
            const double a = a0 + (M_PI / 2.) * i / seg;
            poly.points.emplace_back(scaled(corners[c].x() + r * std::cos(a)), scaled(corners[c].y() + r * std::sin(a)));
        }
    }
    if (r <= 0.) {
        poly.points = {Point(scaled(cx - w / 2), scaled(cy - h / 2)), Point(scaled(cx + w / 2), scaled(cy - h / 2)),
                       Point(scaled(cx + w / 2), scaled(cy + h / 2)), Point(scaled(cx - w / 2), scaled(cy + h / 2))};
    }
    return ExPolygon(poly);
}

// the outline of an object's parts at a height, in plate world coordinates
ExPolygons object_outline(const ModelObject *mo, double z, int only_part)
{
    ExPolygons all;
    for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
        const ModelVolume *mv = mo->volumes[vi];
        if (!mv->is_model_part() || (only_part >= 0 && int(vi) != only_part))
            continue;
        indexed_triangle_set its = mv->mesh().its;
        const Transform3f    t   = (mo->instances.front()->get_matrix() * mv->get_matrix()).cast<float>();
        for (Vec3f &v : its.vertices)
            v = t * v;
        const std::vector<ExPolygons> cut = slice_mesh_ex(its, std::vector<float>{float(z)});
        if (!cut.empty())
            append(all, cut.front());
    }
    return union_ex(all);
}

// one add_part (on the GUI thread)
json add_part_gui(const json &args)
{
    const std::string shape = lower(str(args, "shape", "box"));
    const std::string type  = lower(str(args, "type", "part"));
    if (type != "part" && type != "modifier" && type != "negative")
        throw ToolError("'type' is part, modifier or negative");
    {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        const int    pi      = plate_of_instance(obj_idx, 0);
        const Vec2d  o       = plate_origin(pi);
        const BoundingBoxf3 obj_bb = mo->instance_bounding_box(0);
        Vec2d center(obj_bb.center().x(), obj_bb.center().y());
        if (args.contains("center_on_plate_mm")) {
            const json &c = args["center_on_plate_mm"];
            if (!c.is_array() || c.size() != 2)
                throw ToolError("'center_on_plate_mm' is [x, y] on the plate");
            center = Vec2d(o.x() + c[0].get<double>(), o.y() + c[1].get<double>());
        }
        const double z0 = num(args, "bottom_z_mm", 0.);
        auto size_arg = [&](int n) {
            if (!args.contains("size_mm") || !args["size_mm"].is_array() || int(args["size_mm"].size()) != n)
                throw ToolError(std::string("give 'size_mm' as ") + (n == 3 ? "[x, y, z]" : "[diameter, height]"));
            std::vector<double> v;
            for (const json &e : args["size_mm"])
                v.push_back(e.get<double>());
            for (double e : v)
                if (e <= 0.)
                    throw ToolError("sizes must be above 0");
            return v;
        };
        indexed_triangle_set its;
        std::string          made;
        if (shape == "box" || shape == "rounded_box") {
            const std::vector<double> s = size_arg(3);
            const double r = shape == "rounded_box" ? num(args, "corner_radius_mm", 2.) : 0.;
            its  = extrude_outlines({rounded_rect(s[0], s[1], r, center.x(), center.y())}, z0, z0 + s[2]);
            made = shape + " " + std::to_string(r2(s[0])) + " x " + std::to_string(r2(s[1])) + " x " + std::to_string(r2(s[2])) + " mm";
        } else if (shape == "cylinder") {
            const std::vector<double> s = size_arg(2);
            its = its_make_cylinder(s[0] / 2., s[1], 2. * M_PI / 120.);
            for (Vec3f &v : its.vertices)
                v += Vec3f(float(center.x()), float(center.y()), float(z0));
            made = "cylinder d " + std::to_string(r2(s[0])) + " x " + std::to_string(r2(s[1])) + " mm";
        } else if (shape == "outline") {
            // the outline of an object (or one of its parts) at a height, made into a slab
            const ModelObject *src = mo;
            if (args.contains("outline_of") && !args["outline_of"].is_null())
                src = p->model().objects[find_object(json{{"object", args["outline_of"]}})];
            int src_part = -1;
            if (args.contains("outline_part") && !args["outline_part"].is_null())
                src_part = find_part(src, json{{"part", args["outline_part"]}});
            // by default the middle of the part (or of the object)
            const BoundingBoxf3 sbb = src_part >= 0 ? part_box(src, src->volumes[src_part]) : src->instance_bounding_box(0);
            const double at   = num(args, "outline_at_z_mm", (sbb.min.z() + sbb.max.z()) / 2.);
            const double h    = num(args, "height_mm", 0.);
            const double inset = num(args, "inset_mm", 0.);
            if (h <= 0.)
                throw ToolError("give 'height_mm', the thickness of the slab");
            ExPolygons outline = object_outline(src, at, src_part);
            if (outline.empty())
                throw ToolError("nothing of that object is at z " + std::to_string(r2(at)) + " mm");
            if (inset != 0.)
                outline = offset_ex(outline, float(scale_(-inset)));
            if (outline.empty())
                throw ToolError("the inset leaves nothing");
            if (args.contains("center_on_plate_mm")) {
                const BoundingBox b = get_extents(outline);
                const Vec2d       c = unscale(b.center());
                for (ExPolygon &ex : outline)
                    ex.translate(Point(scaled(center.x() - c.x()), scaled(center.y() - c.y())));
            }
            its  = extrude_outlines(outline, z0, z0 + h);
            made = "outline of '" + src->name + "' at z " + std::to_string(r2(at)) + " mm, inset " + std::to_string(r2(inset)) + " mm, " +
                   std::to_string(r2(h)) + " mm thick";
        } else if (shape == "file") {
            const std::string path = str(args, "path", "");
            const std::string ext  = lower(boost::filesystem::path(path).extension().string());
            if (path.empty() || (ext != ".stl" && ext != ".obj") || !boost::filesystem::exists(path))
                throw ToolError("give 'path', an existing .stl or .obj file");
            Model loaded = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel);
            TriangleMesh m = loaded.mesh();
            const BoundingBoxf3 mb = m.bounding_box();
            m.translate(float(center.x() - mb.center().x()), float(center.y() - mb.center().y()), float(z0 - mb.min.z()));
            its  = m.its;
            made = "from " + boost::filesystem::path(path).filename().string();
        } else
            throw ToolError("'shape' is box, rounded_box, cylinder, outline or file");
        if (its.indices.empty())
            throw ToolError("the shape came out empty");

        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: add part to " + mo->name);
        TriangleMesh mesh(std::move(its));
        // from plate to object coordinates
        mesh.transform(mo->instances.front()->get_matrix().inverse(), true);
        const ModelVolumeType vtype = type == "modifier" ? ModelVolumeType::PARAMETER_MODIFIER :
                                      type == "negative" ? ModelVolumeType::NEGATIVE_VOLUME : ModelVolumeType::MODEL_PART;
        ModelVolume *mv = mo->add_volume(std::move(mesh), vtype);
        mv->name        = str(args, "name", type == "part" ? "Claude part" : type == "modifier" ? "Claude modifier" : "Claude hole");
        if (args.contains("filament") && type != "negative") {
            const int f = check_filament(int(num(args, "filament", 1)), false);
            mv->config.set_key_value("extruder", new ConfigOptionInt(f));
        }
        mo->invalidate_bounding_box();
        if (vtype == ModelVolumeType::MODEL_PART)
            mo->ensure_on_bed(false);
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(obj_idx));
        p->changed_object(obj_idx);
        p->set_plater_dirty(true);
        return {{"added", made}, {"object", mo->name}, {"part_index", int(mo->volumes.size()) - 1}, {"parts", parts_json(obj_idx, mo, pi)}};
    }
}

json tool_add_part(const json &args)
{
    if (!args.contains("parts"))
        return on_gui([args]() -> json { return add_part_gui(args); });
    if (!args["parts"].is_array() || args["parts"].empty())
        throw ToolError("'parts' is a list of parts, each with the same fields as one add_part");
    // several parts in one undo step: each item's fields over the shared ones
    return on_gui([args]() -> json {
        Plater *p = plater();
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: add parts");
        json added = json::array(), last;
        for (const json &item : args["parts"]) {
            if (!item.is_object())
                throw ToolError("each item of 'parts' is an object like one add_part");
            json one = args;
            one.erase("parts");
            for (auto &[k, v] : item.items())
                one[k] = v;
            try {
                last = add_part_gui(one);
            } catch (const std::exception &e) {
                throw ToolError("parts[" + std::to_string(added.size()) + "]: " + e.what() +
                                (added.empty() ? "" : " (the parts before it were added; one undo removes them all)"));
            }
            added.push_back({{"added", last["added"]}, {"object", last["object"]}, {"part_index", last["part_index"]}});
        }
        return {{"added", added}, {"parts", last["parts"]}};
    });
}

json tool_repair(const json &args)
{
    return on_gui([args]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        close_gizmo();
        ObjectList *list = wxGetApp().obj_list();
        list->select_items(std::vector<ObjectVolumeID>{{mo, nullptr}});
        list->fix_through_netfabb();
        // the object may have been replaced
        mo = p->model().objects[std::min(obj_idx, int(p->model().objects.size()) - 1)];
        return {{"object", mo->name}, {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
    }, 300000);
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
        j["filaments"]     = filaments_json();
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
                                "tree_support_branch_diameter", "wall_loops", "sparse_infill_density", "sparse_infill_pattern", "brim_type", "print_sequence",
                                "top_shell_layers", "bottom_shell_layers", "interior_filament", "interior_skin_layers", "enable_prime_tower"})
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
            // filament changes and other custom G-code at layer heights on this plate
            if (auto it = p->model().plates_custom_gcodes.find(i); it != p->model().plates_custom_gcodes.end() && !it->second.gcodes.empty()) {
                json cg = json::array();
                for (const CustomGCode::Item &item : it->second.gcodes)
                    cg.push_back({{"z_mm", r2(item.print_z)}, {"type", int(item.type)}, {"filament", item.extruder}});
                jp["custom_gcodes_at_heights"] = cg;
            }
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
            jo["filament"]  = object_filament(mo);
            size_t tris     = 0;
            bool   enf = false, blk = false, land = false, color = false;
            for (const ModelVolume *mv : mo->volumes) {
                const char *type = mv->is_model_part() ? "part" :
                                   mv->is_negative_volume() ? "negative" :
                                   mv->is_modifier() ? "modifier" :
                                   mv->is_support_enforcer() ? "support_enforcer" :
                                   mv->is_support_blocker() ? "support_blocker" : "other";
                (void) type;
                tris += mv->mesh().its.indices.size();
                if (mv->is_model_part() && !mv->supported_facets.empty()) {
                    enf  = enf || mv->supported_facets.has_facets(*mv, EnforcerBlockerType(1));
                    blk  = blk || mv->supported_facets.has_facets(*mv, EnforcerBlockerType(2));
                    land = land || mv->supported_facets.has_facets(*mv, SUPPORT_LANDING_STATE);
                }
                color = color || !mv->mmu_segmentation_facets.empty();
            }
            jo["parts"]     = parts_json(int(oi), mo, plate_of_instance(int(oi), 0));
            jo["triangles"] = tris;
            if (!mo->layer_config_ranges.empty()) {
                json ranges = json::array();
                for (const auto &[range, cfg] : mo->layer_config_ranges) {
                    json jr = {{"z_mm", json::array({r2(range.first), r2(range.second)})}};
                    for (const std::string &key : cfg.keys())
                        jr[key == "extruder" ? "filament" : key] = cfg.opt_serialize(key);
                    ranges.push_back(jr);
                }
                jo["height_ranges"] = ranges;
            }
            jo["paint"]     = {{"support_enforce", enf}, {"support_block", blk}, {"supports_stand_here", land}, {"color", color}};
            json overrides  = json::object();
            for (const std::string &key : mo->config.keys())
                overrides[key] = mo->config.opt_serialize(key);
            jo["setting_overrides"] = overrides;
            jobjs.push_back(jo);
        }
        j["objects"] = jobjs;
        j["coordinates"] = "mm; x/y on a plate are measured from that plate's front left corner (its origin), z is the height above the plate";
        j["how_filaments_work"] = "A part prints with its own filament (or the object's) where it is not painted; painted triangles print "
                                  "with the painted filament through the paint depth; an object's inside_filament (interior_filament setting) "
                                  "overrides both deeper than interior_skin_layers below top / above bottom surfaces. Use the layers tool after "
                                  "slicing to see which filaments each layer really prints.";
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
        if (args.contains("zoom_box_on_plate_mm") && args["zoom_box_on_plate_mm"].is_array() && args["zoom_box_on_plate_mm"].size() == 4) {
            // a close look at one area: [x_min, y_min, x_max, y_max] on the current plate, over the objects' height
            const json  &b = args["zoom_box_on_plate_mm"];
            const Vec2d  o = plate_origin(p->get_partplate_list().get_curr_plate_index());
            double       top = 1.;
            for (const ModelObject *mo : p->model().objects)
                top = std::max(top, mo->instance_bounding_box(0).max.z());
            p->get_camera().zoom_to_box(BoundingBoxf3(Vec3d(o.x() + b[0].get<double>(), o.y() + b[1].get<double>(), 0.),
                                                      Vec3d(o.x() + b[2].get<double>(), o.y() + b[3].get<double>(), top)));
        } else if (fit == "plate")
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
    const bool clean = !flag(args, "show_popups", false);
    on_gui([promise, clean]() -> json {
        GLCanvas3D *canvas = plater()->get_current_canvas3D();
        if (canvas == nullptr)
            throw ToolError("no 3D view");
        canvas->grab_next_frame([promise](int w, int h, std::vector<uint8_t> &&rgba) {
            try {
                promise->set_value(Frame{w, h, std::move(rgba)});
            } catch (...) {
            }
        }, clean);
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
    std::string note = "MiniMe " + state["screen"].get<std::string>() + " view, camera '" + view + "', " + std::to_string(ow) + "x" + std::to_string(oh) + " px" +
                       (clean ? ", without MiniMe's notification popups (show_popups=true shows them)." : ".");
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
                throw ToolError(key == "extruder" ? std::string("the filament of an object or part is set with the set_filament tool") :
                                                    "'" + key + "' is a printer or filament setting; only process settings can be changed here");
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
        const bool to_parts = lower(str(args, "result", "objects")) == "parts_in_place";
        if (to_parts && keep != "both")
            throw ToolError("result=parts_in_place keeps both pieces (delete one afterwards with delete)");
        ModelObjectCutAttributes attributes = only_if(keep != "lower", ModelObjectCutAttribute::KeepUpper) |
                                              only_if(keep != "upper", ModelObjectCutAttribute::KeepLower) |
                                              only_if(flip && !to_parts, ModelObjectCutAttribute::FlipUpper) |
                                              only_if(to_parts, ModelObjectCutAttribute::CutToParts) |
                                              only_if(true, ModelObjectCutAttribute::InvalidateCutInfo);
        (void) plane;
        close_gizmo();
        const std::string name = mo->name;
        const std::vector<const ModelObject *> before(p->model().objects.begin(), p->model().objects.end());
        Plater::TakeSnapshot snapshot(p, "Claude: cut " + name);
        // the Cut tool's own code: it keeps color and support paint on the pieces (the older ModelObject::cut drops it)
        const Vec3d       inst_off   = mo->instances[inst_idx]->get_offset();
        const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(c.x(), c.y(), z) - inst_off);
        Cut               cut(mo, inst_idx, cut_matrix, attributes);
        cut.set_offset_for_two_part = !to_parts;
        const ModelObjectPtrs &new_objects = cut.perform_with_plane();
        if (cut.was_canceled() || new_objects.empty())
            throw ToolError("the cut made nothing (nothing changed)");
        p->apply_cut_object_to_model(size_t(obj_idx), new_objects);
        p->set_plater_dirty(true);
        json made = json::array();
        Model &model = p->model();
        for (size_t i = 0; i < model.objects.size(); ++i)
            made.push_back({{"index", i}, {"name", model.objects[i]->name}});
        json pieces = json::array();
        for (size_t i = 0; i < model.objects.size(); ++i)
            if (std::find(before.begin(), before.end(), model.objects[i]) == before.end()) {
                const BoundingBoxf3 pb = model.objects[i]->instance_bounding_box(0);
                pieces.push_back({{"index", i}, {"name", model.objects[i]->name}, {"plate", plate_of_instance(int(i), 0)},
                                  {"size_mm", xyz(pb.size())}, {"parts", model.objects[i]->volumes.size()}});
            }
        json j = {{"cut", name}, {"at_height_mm", r2(z)}, {"pieces", pieces}, {"objects_now", made},
                  {"warning", "object indices changed: address objects by name from now on"}};
        if (to_parts) {
            const int last = int(model.objects.size()) - 1;
            int       idx  = std::min(obj_idx, last);
            for (int i = 0; i <= last; ++i)
                if (model.objects[i]->name == name || model.objects[i]->volumes.size() > model.objects[idx]->volumes.size())
                    idx = i;
            j["object"] = idx;
            j["parts"]  = parts_json(idx, model.objects[idx], plate_of_instance(idx, 0));
            j["note"]   = "the pieces are parts of one object, in place";
        } else
            j["note"] = "the pieces are separate objects, set apart on the plate";
        j["paint"] = "color and support paint stay on the pieces";
        return j;
    }, 300000);
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
// color paint and cross-sections

// paint (or erase) color by a rule, or swap one painted filament for another
json tool_paint_color(const json &args)
{
    const std::string mode = lower(str(args, "mode", "set"));
    if (mode != "set" && mode != "replace" && mode != "clear")
        throw ToolError("'mode' is set, replace or clear");
    const std::string facing = lower(str(args, "facing", "any"));
    if (facing != "up" && facing != "down" && facing != "side" && facing != "any")
        throw ToolError("'facing' is up, down, side or any");
    const double within = num(args, "within_deg", facing == "down" ? 30. : 15.);
    const double z_min  = num(args, "z_min_mm", -1e9), z_max = num(args, "z_max_mm", 1e9);
    std::optional<std::array<double, 4>> box;
    if (args.contains("box_on_plate_mm")) {
        const json &b = args["box_on_plate_mm"];
        if (!b.is_array() || b.size() != 4)
            throw ToolError("'box_on_plate_mm' is [x_min, y_min, x_max, y_max]");
        box = std::array<double, 4>{b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>()};
    }
    return on_gui([=]() -> json {
        Plater      *p        = plater();
        const int    obj_idx  = find_object(args);
        ModelObject *mo       = p->model().objects[obj_idx];
        const int    inst_idx = std::clamp(int(num(args, "instance", 0)), 0, int(mo->instances.size()) - 1);
        const int    only     = args.contains("part") && !args["part"].is_null() ? find_part(mo, args) : -1;
        const Vec2d  o        = plate_origin(plate_of_instance(obj_idx, inst_idx));
        int filament = 0, from = 0;
        if (mode == "set")
            filament = check_filament(int(num(args, "filament", -1)), true);
        if (mode == "replace") {
            from     = check_filament(int(num(args, "from_filament", -1)), false);
            filament = check_filament(int(num(args, "filament", -1)), true);
        }
        const double cos_within = std::cos(within * M_PI / 180.), sin_within = std::sin(within * M_PI / 180.);
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: color paint on " + mo->name);
        size_t facets = 0;
        double area   = 0.;
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            ModelVolume *mv = mo->volumes[vi];
            if (!mv->is_model_part() || (only >= 0 && int(vi) != only))
                continue;
            TriangleSelector selector(mv->mesh());
            selector.deserialize(mv->mmu_segmentation_facets.get_data(), true);
            if (mode == "clear") {
                selector.reset();
            } else if (mode == "replace") {
                // loading with "delete filament 'from'" puts 'to' in its place and lowers the numbers above 'from' by one;
                // shifting them back up restores them (and 'to', given one lower when it is above 'from')
                const int to = filament > from ? filament - 1 : filament;
                selector.deserialize(selector.serialize(), true, EnforcerBlockerType::ExtruderMax, EnforcerBlockerType(from), EnforcerBlockerType(to));
                selector.shift_states_above(EnforcerBlockerType(from), 1);
            } else {
                const indexed_triangle_set &its    = mv->mesh().its;
                const Transform3d           trafo  = mo->instances[inst_idx]->get_matrix() * mv->get_matrix();
                const Matrix3d              normal = trafo.linear().inverse().transpose();
                for (int f = 0; f < int(its.indices.size()); ++f) {
                    const Vec3f fa = its.vertices[its.indices[f](0)], fb = its.vertices[its.indices[f](1)], fc = its.vertices[its.indices[f](2)];
                    const Vec3d a = trafo * fa.cast<double>(), b = trafo * fb.cast<double>(), c = trafo * fc.cast<double>();
                    Vec3d       n = normal * (fb - fa).cross(fc - fa).cast<double>();
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
                    selector.set_facet(f, EnforcerBlockerType(filament));
                    ++facets;
                    area += 0.5 * (b - a).cross(c - a).norm();
                }
            }
            mv->mmu_segmentation_facets.set(selector);
        }
        p->changed_object(obj_idx);
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        p->set_plater_dirty(true);
        p->schedule_background_process();
        json j = {{"object", mo->name}, {"mode", mode}, {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
        if (mode == "set") {
            j["triangles"] = facets;
            j["area_mm2"]  = r2(area);
            if (facets == 0)
                j["note"] = "nothing matched: loosen facing / within_deg / z / box";
        }
        return j;
    });
}

// where a line crosses the outlines: the stretches of the line inside material
json line_intervals(const ExPolygons &polys, bool along_first, double at)
{
    std::vector<double> hits;
    auto edges = [&](const Polygon &poly) {
        const size_t n = poly.points.size();
        for (size_t i = 0; i < n; ++i) {
            const Vec2d a = unscale(poly.points[i]), b = unscale(poly.points[(i + 1) % n]);
            // the line is "second coordinate = at" (along the first axis) or the other way round
            const double a_fix = along_first ? a.y() : a.x(), b_fix = along_first ? b.y() : b.x();
            const double a_run = along_first ? a.x() : a.y(), b_run = along_first ? b.x() : b.y();
            if ((a_fix <= at && b_fix > at) || (b_fix <= at && a_fix > at))
                hits.push_back(a_run + (at - a_fix) / (b_fix - a_fix) * (b_run - a_run));
        }
    };
    for (const ExPolygon &ex : polys) {
        edges(ex.contour);
        for (const Polygon &h : ex.holes)
            edges(h);
    }
    std::sort(hits.begin(), hits.end());
    json out = json::array();
    for (size_t i = 0; i + 1 < hits.size(); i += 2)
        out.push_back({{"from", r2(hits[i])}, {"to", r2(hits[i + 1])}, {"length", r2(hits[i + 1] - hits[i])}});
    return out;
}

// a cross-section of an object: the outline where a plane cuts it, to measure slots, walls, lips and heights
json tool_section(const json &args)
{
    const std::string plane = lower(str(args, "plane", "z"));
    if (plane != "z" && plane != "x" && plane != "y")
        throw ToolError("'plane' is z (horizontal, at a height), x (vertical, at an x on the plate) or y (vertical, at a y)");
    if (!args.contains("at_mm"))
        throw ToolError("give 'at_mm', where the plane is: a height for z, a plate x for x, a plate y for y");
    const double at        = num(args, "at_mm", 0.);
    double       tolerance = std::max(0.01, num(args, "detail_mm", 0.05));
    return on_gui([=]() mutable -> json {
        Plater      *p        = plater();
        const int    obj_idx  = find_object(args);
        ModelObject *mo       = p->model().objects[obj_idx];
        const int    only     = args.contains("part") && !args["part"].is_null() ? find_part(mo, args) : -1;
        const int    pi       = plate_of_instance(obj_idx, 0);
        const Vec2d  o        = plate_origin(pi);
        // plate coordinates, turned so that the plane is horizontal: x -> (y, z) at x; y -> (z, x) at y (both keep the
        // mesh's orientation)
        ExPolygons all;
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            const ModelVolume *mv = mo->volumes[vi];
            if (!mv->is_model_part() || (only >= 0 && int(vi) != only))
                continue;
            indexed_triangle_set its = mv->mesh().its;
            const Transform3d    t   = mo->instances.front()->get_matrix() * mv->get_matrix();
            for (Vec3f &v : its.vertices) {
                const Vec3d w = t * v.cast<double>() - Vec3d(o.x(), o.y(), 0.);
                v = plane == "z" ? w.cast<float>() : plane == "x" ? Vec3f(float(w.y()), float(w.z()), float(w.x())) :
                                                                    Vec3f(float(w.z()), float(w.x()), float(w.y()));
            }
            const std::vector<ExPolygons> cut = slice_mesh_ex(its, std::vector<float>{float(at)});
            if (!cut.empty())
                append(all, cut.front());
        }
        all = union_ex(all);
        if (all.empty())
            throw ToolError("the plane does not cross the object there");
        // back to (first axis, second axis) as named below
        if (plane == "y")
            for (ExPolygon &ex : all) {
                auto swap = [](Polygon &poly) {
                    for (Point &pt : poly.points)
                        std::swap(pt.x(), pt.y());
                    poly.reverse();
                };
                swap(ex.contour);
                for (Polygon &h : ex.holes)
                    swap(h);
            }
        const std::array<const char *, 2> axes = plane == "z" ? std::array<const char *, 2>{"x", "y"} :
                                                 plane == "x" ? std::array<const char *, 2>{"y", "z"} : std::array<const char *, 2>{"x", "z"};
        // fewer points until it is a readable size
        ExPolygons simple;
        size_t     points = 0;
        for (int round = 0; round < 6; ++round, tolerance *= 2.) {
            simple.clear();
            for (const ExPolygon &ex : all)
                append(simple, ex.simplify(scaled(tolerance)));
            points = 0;
            for (const ExPolygon &ex : simple) {
                points += ex.contour.points.size();
                for (const Polygon &h : ex.holes)
                    points += h.points.size();
            }
            if (points <= 1500)
                break;
        }
        auto pts = [](const Polygon &poly) {
            json a = json::array();
            for (const Point &pt : poly.points)
                a.push_back(json::array({r2(unscale<double>(pt.x())), r2(unscale<double>(pt.y()))}));
            return a;
        };
        json shapes = json::array();
        for (const ExPolygon &ex : simple) {
            const BoundingBox bb = get_extents(ex);
            json s = {{"area_mm2", r2(ex.area() * SCALING_FACTOR * SCALING_FACTOR)},
                      {"min", json::array({r2(unscale<double>(bb.min.x())), r2(unscale<double>(bb.min.y()))})},
                      {"max", json::array({r2(unscale<double>(bb.max.x())), r2(unscale<double>(bb.max.y()))})},
                      {"outline", pts(ex.contour)}};
            if (!ex.holes.empty()) {
                json holes = json::array();
                for (const Polygon &h : ex.holes)
                    holes.push_back(pts(h));
                s["holes"] = holes;
            }
            shapes.push_back(s);
        }
        json j = {{"object", mo->name}, {"plane", plane}, {"at_mm", r2(at)}, {"axes", json::array({axes[0], axes[1]})},
                  {"detail_mm", r2(tolerance)}, {"shapes", shapes},
                  {"note", std::string("points are [") + axes[0] + ", " + axes[1] + "] in mm, plate coordinates (z = height)"}};
        // optional measuring lines across the section
        if (args.contains("lines") && args["lines"].is_array()) {
            json measured = json::array();
            for (const json &l : args["lines"]) {
                const std::string axis = l.is_object() ? lower(l.value("along", "")) : "";
                if (!l.is_object() || (axis != axes[0] && axis != axes[1]) || !l.contains("at") || !l["at"].is_number())
                    throw ToolError(std::string("each line is {\"along\": \"") + axes[0] + "\" or \"" + axes[1] + "\", \"at\": number}");
                const bool along_first = axis == axes[0];
                measured.push_back({{"along", axis}, {"at", r2(l["at"].get<double>())},
                                    {"inside_material", line_intervals(all, along_first, l["at"].get<double>())}});
            }
            j["lines"] = measured;
        }
        return j;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// copies and mirrors

Axis axis_arg(const std::string &a)
{
    const std::string s = lower(a);
    if (s == "x")
        return X;
    if (s == "y")
        return Y;
    if (s == "z")
        return Z;
    throw ToolError("'axis' is x, y or z");
}

// mirror across the plane axis = w (world coordinates)
Transform3d mirror_across(Axis axis, double w)
{
    Vec3d at = Vec3d::Zero();
    at[axis] = w;
    Transform3d s = Transform3d::Identity();
    s(int(axis), int(axis)) = -1.;
    return Geometry::translation_transform(at) * s * Geometry::translation_transform(-at);
}

json tool_duplicate_object(const json &args)
{
    return on_gui([args]() -> json {
        Plater        *p       = plater();
        Model         &model   = p->model();
        const int      obj_idx = find_object(args);
        ModelObject   *mo      = model.objects[obj_idx];
        PartPlateList &plates  = p->get_partplate_list();
        const int      src_pi  = plate_of_instance(obj_idx, 0);
        const int      pi      = args.contains("plate") && !args["plate"].is_null() ? plate_arg(args) : src_pi;
        const BoundingBoxf3 bb = mo->instance_bounding_box(0);
        Vec2d target;
        if (args.contains("center_on_plate_mm")) {
            const json &c = args["center_on_plate_mm"];
            if (!c.is_array() || c.size() != 2)
                throw ToolError("'center_on_plate_mm' is [x, y] on the plate");
            target = plate_origin(pi) + Vec2d(c[0].get<double>(), c[1].get<double>());
        } else {
            // the nearest free spot on that plate
            const Vec3d start = pi == src_pi ? bb.center() : plates.get_plate(pi)->get_build_volume().center();
            const Vec2f cell  = p->canvas3D()->get_nearest_empty_cell(Vec2f(float(start.x()), float(start.y())),
                                                                      Vec2f(float(bb.size().x() + 1.), float(bb.size().y() + 1.)));
            target = cell.cast<double>();
        }
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: copy " + mo->name);
        ModelObject *dst = model.add_object(*mo);
        for (ModelVolume *mv : dst->volumes)
            if (mv->is_model_part())
                mv->ensure_part_guid(true);
        dst->name = str(args, "name", mo->name + " copy");
        const Vec3d d(target.x() - bb.center().x(), target.y() - bb.center().y(), 0.);
        for (ModelInstance *inst : dst->instances)
            inst->set_offset(inst->get_offset() + d);
        model.set_assembly_pos(dst);
        const size_t new_idx = model.objects.size() - 1;
        wxGetApp().obj_list()->paste_objects_into_list({new_idx});
        p->set_plater_dirty(true);
        const BoundingBoxf3 nb  = dst->instance_bounding_box(0);
        const int           npi = plate_of_instance(int(new_idx), 0);
        const Vec2d         o   = plate_origin(npi);
        return {{"copied", mo->name}, {"new_object", {{"index", new_idx}, {"name", dst->name}}}, {"plate", npi},
                {"center_on_plate_mm", xy(Vec2d(nb.center().x() - o.x(), nb.center().y() - o.y()))}, {"size_mm", xyz(nb.size())},
                {"note", "the copy has every part, filament, paint and setting of the original; address objects by name"}};
    });
}

json tool_mirror(const json &args)
{
    const Axis axis = axis_arg(str(args, "axis", ""));
    return on_gui([args, axis]() -> json {
        Plater      *p       = plater();
        const int    obj_idx = find_object(args);
        ModelObject *mo      = p->model().objects[obj_idx];
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: mirror " + mo->name);
        std::string what;
        if (args.contains("part") && !args["part"].is_null()) {
            // one part, in place: across the plane through its own middle
            const int         vi   = find_part(mo, args);
            ModelVolume      *mv   = mo->volumes[vi];
            const Transform3d inst = mo->instances.front()->get_matrix();
            const Vec3d       c    = part_box(mo, mv).center();
            mv->set_transformation(Geometry::Transformation(Transform3d(inst.inverse() * mirror_across(axis, c[axis]) * inst * mv->get_matrix())));
            what = "part '" + mv->name + "'";
        } else {
            for (size_t i = 0; i < mo->instances.size(); ++i) {
                ModelInstance *inst = mo->instances[i];
                const Vec3d    c    = mo->instance_bounding_box(i).center();
                inst->set_transformation(Geometry::Transformation(Transform3d(mirror_across(axis, c[axis]) * inst->get_matrix())));
            }
            what = "object";
        }
        mo->invalidate_bounding_box();
        mo->ensure_on_bed(false);
        p->changed_object(obj_idx);
        wxGetApp().obj_list()->update_info_items(size_t(obj_idx));
        p->set_plater_dirty(true);
        return {{"mirrored", what}, {"object", mo->name}, {"axis", std::string(1, "xyz"[int(axis)])},
                {"parts", parts_json(obj_idx, mo, plate_of_instance(obj_idx, 0))}};
    });
}

json tool_copy_parts(const json &args)
{
    if (!args.contains("parts") || !args["parts"].is_array() || args["parts"].empty())
        throw ToolError("give 'parts', the parts to copy (index or name)");
    return on_gui([args]() -> json {
        Plater      *p     = plater();
        Model       &model = p->model();
        const int    a     = find_object(args);
        const int    b     = args.contains("to_object") && !args["to_object"].is_null() ? find_object(args, "to_object") : a;
        ModelObject *A = model.objects[a], *B = model.objects[b];
        std::vector<ModelVolume *> src;
        for (const json &e : args["parts"]) {
            ModelVolume *v = A->volumes[find_part(A, json{{"part", e}})];
            if (std::find(src.begin(), src.end(), v) == src.end())
                src.push_back(v);
        }
        // the change, in plate (world) coordinates
        Transform3d t = Transform3d::Identity();
        if (args.contains("mirror") && !args["mirror"].is_null()) {
            const json &m = args["mirror"];
            if (!m.is_object() || !m.contains("at_mm") || !m["at_mm"].is_number())
                throw ToolError("'mirror' is {\"axis\": \"x\" | \"y\" | \"z\", \"at_mm\": the plate coordinate (or height) of the mirror plane}");
            const Axis   axis = axis_arg(m.value("axis", ""));
            const Vec2d  o    = plate_origin(plate_of_instance(a, 0));
            const double at   = m["at_mm"].get<double>();
            t = mirror_across(axis, axis == X ? o.x() + at : axis == Y ? o.y() + at : at);
        }
        if (args.contains("move_by_mm")) {
            const json &mv = args["move_by_mm"];
            if (!mv.is_array() || mv.size() < 2)
                throw ToolError("'move_by_mm' is [x, y] or [x, y, z]");
            t = Geometry::translation_transform(Vec3d(mv[0].get<double>(), mv[1].get<double>(), mv.size() > 2 ? mv[2].get<double>() : 0.)) * t;
        }
        close_gizmo();
        Plater::TakeSnapshot snapshot(p, "Claude: copy parts to " + B->name);
        const Transform3d ia = A->instances.front()->get_matrix();
        const Transform3d ib = B->instances.front()->get_matrix().inverse();
        json made = json::array();
        for (ModelVolume *sv : src) {
            const Transform3d m  = ib * t * ia * sv->get_matrix();
            ModelVolume      *nv = B->add_volume(*sv);
            if (nv->is_model_part())
                nv->ensure_part_guid(true);
            nv->set_transformation(Geometry::Transformation(m));
            nv->name = sv->name + (a == b ? " copy" : "");
            made.push_back({{"part_index", int(B->volumes.size()) - 1}, {"name", nv->name}, {"from", sv->name}});
        }
        B->invalidate_bounding_box();
        B->ensure_on_bed(false);
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(b));
        p->changed_object(b);
        p->set_plater_dirty(true);
        return {{"copied", made}, {"to_object", B->name}, {"parts", parts_json(b, B, plate_of_instance(b, 0))},
                {"note", "copies keep type, filament, paint and settings; check them with get_scene or section"}};
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// pricing: the same calculation as the user's pricing artifact, from the sliced plate

json tool_price(const json &args)
{
    return on_gui([args]() -> json {
        json       data  = Pricing::load();
        const int  plate = args.contains("plate") && !args["plate"].is_null() ? plate_arg(args) : -1;
        const json usage = Pricing::plate_usage(data, plate);
        json       ov    = args;
        ov.erase("plate");
        json j = Pricing::compute(data, usage, ov);
        j["plate"]   = usage["plate"].get<int>() - 1;
        j["project"] = usage["project"];
        j["print_time"] = format_time(usage["seconds"].get<double>());
        j["note"] = "money in R$; margins are R$ per print hour; scenario prices already include the channel fee";
        return j;
    });
}

json tool_pricing(const json &args)
{
    return on_gui([args]() -> json {
        json              data   = Pricing::load();
        const std::string action = lower(str(args, "action", "get"));
        json              out    = json::object();
        if (action != "get") {
            out = Pricing::apply_change(data, args);
            Pricing::refresh_window();
        }
        json filaments = json::array();
        for (json f : data["filaments"]) {
            const double kg = f.value("spool_kg", 0.);
            f["price_kg"]   = kg > 0. ? r2(f.value("paid", 0.) / kg) : 0.;
            filaments.push_back(f);
        }
        out["config"]    = data["config"];
        out["filaments"] = filaments;
        // which list filament each MiniMe filament uses
        json slots = json::array();
        PresetBundle *pb = wxGetApp().preset_bundle;
        for (size_t i = 0; i < pb->filament_presets.size(); ++i) {
            const std::string key = Pricing::slot_key(int(i) + 1);
            json s = {{"filament", int(i) + 1}, {"preset", pb->filament_presets[i]}};
            if (data["slots"].contains(key))
                s["assigned"] = data["slots"][key];
            slots.push_back(s);
        }
        out["minime_filaments"] = slots;
        return out;
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// what the user did in MiniMe between Claude's calls (undo steps that are not Claude's)

size_t s_seen_time   = 0;
size_t s_seen_active = 0;
bool   s_seen        = false;

json user_changes()
{
    json out = json::array();
    if (!s_seen)
        return out;
    const UndoRedo::Stack &st = plater()->undo_redo_stack_main();
    for (const UndoRedo::Snapshot &snap : st.snapshots())
        if (snap.timestamp > s_seen_time && !snap.name.empty() && snap.name.rfind("Claude", 0) != 0 && snap.name.rfind("Internal", 0) != 0)
            out.push_back(snap.name);
    if (out.empty() && st.active_snapshot_time() != s_seen_active)
        out.push_back("undo or redo");
    return out;
}

void mark_seen()
{
    const UndoRedo::Stack &st = plater()->undo_redo_stack_main();
    for (const UndoRedo::Snapshot &snap : st.snapshots())
        s_seen_time = std::max(s_seen_time, snap.timestamp);
    s_seen_active = st.active_snapshot_time();
    s_seen        = true;
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
            "What is in MiniMe now: the filaments (number, colour, type), plates (support zones, filament changes at heights), objects "
            "with position, size, rotation, scale, filament, setting overrides and height ranges, and every part of every object with its "
            "type, filament, box and z range on the plate, painted filaments (with their z range) and mesh errors. Call this first, and "
            "again after changes to check them.",
            R"J({"type": "object", "properties": {}})J", tool_get_scene);
        add("render_view", "A picture of MiniMe's 3D view or of the sliced preview (toolpaths, supports) as MiniMe draws it. Use it to check results.",
            R"J({"type": "object", "properties": {
                "view": {"type": "string", "enum": ["iso", "top", "bottom", "front", "rear", "left", "right", "keep"], "description": "camera direction; keep = as the user left it"},
                "screen": {"type": "string", "enum": ["3d", "preview", "keep"], "description": "3d = the models; preview = the sliced result"},
                "fit": {"type": "string", "enum": ["plate", "objects", "keep"]},
                "max_width": {"type": "integer", "description": "image width limit in pixels, default 1280"},
                "zoom_box_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x_min, y_min, x_max, y_max]: a close look at that area of the current plate"},
                "show_popups": {"type": "boolean", "description": "include MiniMe's notification popups (default false: they are left out so they cover nothing)"}}})J",
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
            "Cuts an object with a flat horizontal cut at a height (one undo step). Color and support paint stay on the pieces. With "
            "result=parts_in_place the pieces stay in place as parts of the same object (to give a layer band its own filament). Only "
            "flat horizontal cuts: for angled, curved or shaped cuts and connectors the user uses the Cut tool.",
            R"J({"type": "object", "properties": {
                "object": $OBJ,
                "height_mm": {"type": "number", "description": "height above the plate"},
                "keep": {"type": "string", "enum": ["both", "upper", "lower"]},
                "flip_upper": {"type": "boolean", "description": "turn the upper part over so it stands on the cut face"},
                "result": {"type": "string", "enum": ["objects", "parts_in_place"], "description": "objects (default) = separate objects set on the plate; parts_in_place = both pieces stay where they are, as parts of one object"},
                "instance": {"type": "integer"}}, "required": ["object", "height_mm"]})J",
            tool_cut_object);
        add("add_model", "Adds the objects of an .stl or .obj file to the current plate (one undo step). It never replaces the project.",
            R"J({"type": "object", "properties": {"path": {"type": "string", "description": "full path of the file on this computer"}}, "required": ["path"]})J",
            tool_add_model);
        add("select_plate", "Makes a plate the current one.", R"J({"type": "object", "properties": {"plate": {"type": "integer"}}, "required": ["plate"]})J",
            tool_select_plate);
        add("undo", "Undoes (or with redo=true, redoes) the last steps in MiniMe, including the user's own.",
            R"J({"type": "object", "properties": {"steps": {"type": "integer"}, "redo": {"type": "boolean"}}})J", tool_undo);
        add("layers",
            "After slice: what the current plate prints layer by layer - the filaments of each layer in print order (runs of equal layers "
            "grouped), the number of filament changes, layers per filament and filament changes at heights. Use it instead of pictures to "
            "check colors per layer and to count filament changes.",
            R"J({"type": "object", "properties": {"max_rows": {"type": "integer", "description": "most rows to return, default 80"}}})J", tool_layers);
        add("set_filament",
            "Sets the filament of an object or of one part (one undo step). Setting the object's filament also sets its parts to it; set "
            "parts after that. A part with filament 0 follows the object. Painted areas keep their paint.",
            R"J({"type": "object", "properties": {"object": $OBJ, "part": {"type": ["integer", "string"], "description": "part index or name (get_scene lists them); leave out for the whole object"},
                "filament": {"type": "integer", "description": "filament number, 1-based as in MiniMe"}}, "required": ["object", "filament"]})J", tool_set_filament);
        add("assemble_objects",
            "Joins several objects into one object with several parts, keeping every part where it is (one undo step). Put the pieces in "
            "place first (transform_object), then assemble; inside one object, parts can sit at any height (move_part).",
            R"J({"type": "object", "properties": {"objects": {"type": "array", "items": {"type": ["integer", "string"]}, "description": "objects to join (index or name), at least two"}}, "required": ["objects"]})J",
            tool_assemble);
        add("move_part",
            "Moves (or resizes) one part inside an object (one undo step): put its bottom at a height, its middle at a place on the plate, "
            "or move it by an amount. The object as a whole stays on the plate.",
            R"J({"type": "object", "properties": {"object": $OBJ, "part": {"type": ["integer", "string"]},
                "bottom_z_mm": {"type": "number", "description": "height of the part's bottom above the plate"},
                "center_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y]"},
                "move_by_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y] or [x, y, z]"},
                "size_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y, z] new size; 0 keeps that size"}}, "required": ["object", "part"]})J",
            tool_move_part);
        add("delete", "Deletes an object, or with 'part' one part of it, or with 'parts' several parts at once (one undo step). Object "
            "indices change after an object is deleted: address objects by name.",
            R"J({"type": "object", "properties": {"object": $OBJ, "part": {"type": ["integer", "string"]},
                "parts": {"type": "array", "items": {"type": ["integer", "string"]}, "description": "several parts (index or name)"}}, "required": ["object"]})J", tool_delete);
        add("duplicate_object",
            "Copies an object with all its parts, filaments, paint and settings (one undo step), onto the same plate or another one "
            "(plate index), at a place or the nearest free spot. Use it to try a new version without losing the current one.",
            R"J({"type": "object", "properties": {"object": $OBJ,
                "plate": {"type": "integer", "description": "plate index for the copy; default the original's plate"},
                "center_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y] on that plate; default the nearest free spot"},
                "name": {"type": "string"}}, "required": ["object"]})J",
            tool_duplicate_object);
        add("mirror",
            "Mirrors an object, or one part of it in place (e.g. reversed text), along a plate axis (one undo step). x flips left-right, "
            "y front-back, z upside down.",
            R"J({"type": "object", "properties": {"object": $OBJ, "part": {"type": ["integer", "string"], "description": "only this part; leave out for the whole object"},
                "axis": {"type": "string", "enum": ["x", "y", "z"]}}, "required": ["object", "axis"]})J",
            tool_mirror);
        add("copy_parts",
            "Copies parts (solid, negative or modifier, with filament, paint and settings) within an object or into another object "
            "(one undo step), optionally mirrored across a plane and/or moved. Use it to make one side match the other: e.g. "
            "mirror across z at the middle height, or across y at a plate y, then move_by_mm.",
            R"J({"type": "object", "properties": {"object": $OBJ,
                "parts": {"type": "array", "items": {"type": ["integer", "string"]}, "description": "parts of 'object' to copy (index or name)"},
                "to_object": {"type": ["integer", "string"], "description": "the object to put the copies in; default the same object"},
                "mirror": {"type": "object", "description": "{'axis': 'x'|'y'|'z', 'at_mm': plate x / plate y / height of the mirror plane}"},
                "move_by_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y] or [x, y, z], after the mirror"}}, "required": ["object", "parts"]})J",
            tool_copy_parts);
        add("add_part",
            "Adds a part (or a modifier or a negative part) to an object at an exact place (one undo step): a box, a rounded box, a "
            "cylinder, a slab with the outline of an object or part at a height (optionally inset), or an .stl/.obj file. Give it a "
            "filament. Use this to make a core, a plate or a backing in another color instead of writing files. With 'parts', adds "
            "several in one undo step (e.g. boxes, pins and their holes).",
            R"J({"type": "object", "properties": {"object": $OBJ,
                "shape": {"type": "string", "enum": ["box", "rounded_box", "cylinder", "outline", "file"]},
                "type": {"type": "string", "enum": ["part", "modifier", "negative"]},
                "size_mm": {"type": "array", "items": {"type": "number"}, "description": "box/rounded_box: [x, y, z]; cylinder: [diameter, height]"},
                "corner_radius_mm": {"type": "number", "description": "rounded_box corners"},
                "outline_of": {"type": ["integer", "string"], "description": "outline: the object to take the outline from (default: this object)"},
                "outline_part": {"type": ["integer", "string"], "description": "outline: only this part of it"},
                "outline_at_z_mm": {"type": "number", "description": "outline: the height to take it at (default: the middle of the part, or of the object)"},
                "inset_mm": {"type": "number", "description": "outline: shrink it by this much (negative grows it)"},
                "height_mm": {"type": "number", "description": "outline: thickness of the slab"},
                "path": {"type": "string", "description": "file: an .stl or .obj"},
                "center_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x, y]; default the object's middle (outline: where it is)"},
                "bottom_z_mm": {"type": "number", "description": "height of its bottom, default 0"},
                "filament": {"type": "integer"},
                "name": {"type": "string"},
                "parts": {"type": "array", "items": {"type": "object"}, "description": "several parts in one undo step: each item has the fields of one part; fields given outside 'parts' apply to all"}}, "required": ["object"]})J",
            tool_add_part);
        add("paint_color",
            "Color paint on an object's surface (one undo step). mode=set paints whole triangles chosen by a rule (which way they face, "
            "height range, box on the plate, one part) with a filament, or erases paint with filament 0 (the part then prints in its own "
            "filament there). mode=replace swaps every triangle painted with from_filament for filament (0 erases it), keeping the "
            "painted shapes exactly. mode=clear removes all color paint. get_scene shows painted filaments per part and their heights.",
            R"J({"type": "object", "properties": {"object": $OBJ,
                "mode": {"type": "string", "enum": ["set", "replace", "clear"]},
                "filament": {"type": "integer", "description": "filament to paint with; 0 erases"},
                "from_filament": {"type": "integer", "description": "replace: the painted filament to swap"},
                "part": {"type": ["integer", "string"], "description": "only this part"},
                "facing": {"type": "string", "enum": ["up", "down", "side", "any"], "description": "set: up = top faces, down = bottoms, side = walls"},
                "within_deg": {"type": "number", "description": "set: how far from straight up/down (or from vertical for side) a face may tilt"},
                "z_min_mm": {"type": "number"},
                "z_max_mm": {"type": "number"},
                "box_on_plate_mm": {"type": "array", "items": {"type": "number"}, "description": "[x_min, y_min, x_max, y_max]"}}, "required": ["object", "mode"]})J",
            tool_paint_color);
        add("section",
            "Measures an object: the outline where a plane cuts it, with exact coordinates. plane=z cuts at a height (a layer seen from "
            "above); plane=x or y cuts vertically at that plate coordinate (a side profile: slot widths, lip heights, wall thickness, "
            "angles). 'lines' measures across the section: where a straight line runs through material (gaps between them are slots "
            "or holes).",
            R"J({"type": "object", "properties": {"object": $OBJ,
                "plane": {"type": "string", "enum": ["z", "x", "y"]},
                "at_mm": {"type": "number", "description": "height for z; plate x for x; plate y for y"},
                "part": {"type": ["integer", "string"], "description": "only this part"},
                "detail_mm": {"type": "number", "description": "how much the outline may be simplified, default 0.05"},
                "lines": {"type": "array", "items": {"type": "object"}, "description": "e.g. [{'along': 'y', 'at': 12}] = the line z=12 running along y"}}, "required": ["object", "plane", "at_mm"]})J",
            tool_section);
        add("price",
            "What to charge for the sliced plate, with the user's own pricing (the same calculation as their pricing artifact): "
            "machine cost per hour x print hours + filament cost (grams of every filament from the slice, including purge and prime "
            "tower, x the price per kg from the user's filament list) + losses %, + packaging + shipping; then for the low / medium / "
            "high (and custom) profit margins in R$ per print hour the price, profit and total cost, with the channel fee (% of the "
            "price or a fixed R$) included. Also the break-even price (no profit) and, with sell_price, whether that price is worth it "
            "(net profit, profit per hour, verdict). Slice first. Any setting given here changes only this calculation; the user's "
            "saved settings change with the pricing tool. The user also has a Price window (the Price button at the top).",
            R"J({"type": "object", "properties": {
                "plate": {"type": "integer", "description": "plate index as get_scene lists it; default the current one"},
                "sell_price": {"type": "number", "description": "a price to judge, in R$"},
                "scenario": {"type": "string", "enum": ["baixa", "media", "alta", "personalizada"]},
                "custom_margin": {"type": "number", "description": "R$ per print hour for scenario personalizada; 0 = no profit"},
                "machine_per_hour": {"type": "number"}, "packaging": {"type": "number"}, "shipping": {"type": "number"},
                "fee_type": {"type": "string", "enum": ["pct", "brl"]}, "fee_value": {"type": "number"},
                "waste_pct": {"type": "number", "description": "losses and failed prints, % of the production cost"},
                "hours": {"type": "number", "description": "use these hours instead of the sliced print time"},
                "prices": {"type": "object", "description": "price per kg for this calculation, by MiniMe filament number, e.g. {'1': 95}"}}})J",
            tool_price);
        add("pricing",
            "The user's saved pricing data (shared with the Price window): settings, their filament list (brand, type, colour, spool "
            "kg, price paid -> price per kg) and which list filament each MiniMe filament uses. action=get reads it; set_config saves "
            "settings (machine_per_hour, margin_low, margin_mid, margin_high in R$ per print hour, packaging, shipping, fee_type, "
            "fee_value, scenario, custom_margin, waste_pct, artifact_url); save_filament adds or (with id) changes one filament; "
            "import_filaments adds many (same brand+type+colour updates); delete_filament; assign ties a MiniMe filament number (slot) "
            "to a list filament (filament=id) or a price per kg (price_kg), or back to automatic (neither). Change saved data only when "
            "the user asks.",
            R"J({"type": "object", "properties": {
                "action": {"type": "string", "enum": ["get", "set_config", "save_filament", "import_filaments", "delete_filament", "assign"]},
                "values": {"type": "object", "description": "set_config: the settings to save"},
                "filament": {"type": ["object", "string"], "description": "save_filament: {id?, brand, type, color, spool_kg, paid}; assign: a list filament id"},
                "filaments": {"type": "array", "items": {"type": "object"}, "description": "import_filaments: [{brand, type, color, spool_kg, paid}]"},
                "id": {"type": "string", "description": "delete_filament: its id"},
                "slot": {"type": "integer", "description": "assign: the MiniMe filament number"},
                "price_kg": {"type": "number", "description": "assign: a price per kg instead of a list filament"}}, "required": ["action"]})J",
            tool_pricing);
        add("repair_object", "Repairs an object's mesh errors (open edges, flipped faces), as Fix model does. get_scene shows mesh errors per part.",
            R"J({"type": "object", "properties": {"object": $OBJ}, "required": ["object"]})J", tool_repair);
        return l;
    }();
    return list;
}

const char *INSTRUCTIONS =
    "MiniMe Slicer (a Bambu Studio fork for FDM 3D printing) is open on the user's computer. These tools work on it. Lengths are "
    "millimetres; x/y on a plate are measured from its front left corner, z is the height above the plate. Each change is one undo "
    "step (undo tool, or Ctrl+Z for the user). You cannot print or send to a printer.\n"
    "\n"
    "WORK FROM DATA. get_scene first. After every change read the result back (get_scene, section, or slice + layers) and check the "
    "numbers before you say it worked; never say something is done that a tool did not confirm. render_view is for showing the user, "
    "or for what no number tells.\n"
    "\n"
    "WHAT YOU CAN DO\n"
    "- Look: get_scene (filaments; plates; objects; every part with type, filament, box and z range, painted filaments with their "
    "heights, mesh errors, setting overrides; height ranges). section measures the model itself: the outline where a horizontal "
    "(z) or vertical (x / y) plane cuts it, with 'lines' giving the material along a line - use it for slot widths, lip heights, wall "
    "thickness, angles, how flat a face is. render_view shows the 3D view or the sliced preview.\n"
    "- Slice and check: slice (time, grams, warnings, supports); layers (filaments of every layer in print order, filament changes).\n"
    "- Settings: find_settings, get_settings, set_settings (whole project, or one object's overrides). Inside filament: set the "
    "object's interior_filament and interior_skin_layers - everything deeper than that many layers below top / above bottom "
    "surfaces prints in one filament, over paint and parts, so colors stay on the outer layers (fewer filament changes).\n"
    "- Objects: transform_object (move, turn, scale, move to another plate; objects always stand on the plate), duplicate_object "
    "(a full copy, on any plate: keep the current version and try a new one on the copy), mirror (an object or one part), "
    "add_model (.stl/.obj), delete, assemble_objects (join objects into one, keeping places), select_plate, repair_object.\n"
    "- Parts inside an object: move_part (any height; resize), set_filament, delete, add_part: box, rounded box, cylinder, a slab "
    "with the outline of an object or part at a height (inset or grown), or a file, as a part (adds material, with its own filament), "
    "a negative part (SUBTRACTS its shape: holes, slots, cable cuts, flattening a face) or a modifier (other settings inside it). "
    "add_part with 'parts' adds many in one undo step; delete with 'parts' removes many. copy_parts copies parts within an object "
    "or into another one, mirrored and/or moved: use it to make two sides match exactly. Glue-together pieces: negative cylinders "
    "as holes in both faces and loose pins as a separate small object (pins stand up on the plate, no supports).\n"
    "- Cut: cut_object, a flat horizontal cut at a height; paint stays on the pieces; result=parts_in_place keeps the pieces in place "
    "as parts of one object (e.g. a top band in another filament).\n"
    "- Color paint: paint_color - mode=set paints whole triangles chosen by facing (up / down / side), height, box, part with a "
    "filament (0 erases); mode=replace swaps one painted filament for another everywhere, keeping the painted shapes exactly; "
    "mode=clear. Typical: erase or recolor paint on side walls (facing=side) so colors stay on the faces.\n"
    "- Supports: support settings, paint_supports (enforce / block / stand_here on surfaces by rule), support_zone (no-support and "
    "supports-only-here areas on the plate), support_report after slicing.\n"
    "- Price: price gives what to charge for a sliced plate with the user's own costs and profit margins (R$), the break-even "
    "price and whether a given price is worth it; pricing reads or (when the user asks) changes their saved costs and filament "
    "list. When you compare versions, give the price difference too.\n"
    "\n"
    "HOW TO WORK\n"
    "- Indices of objects change after cut, delete and duplicate: address objects by name, and read the names a tool returns.\n"
    "- Before a change meant to save time, filament or money, slice the current state first so you can compare before and after.\n"
    "- If a tool result says the user changed things since your last call, call get_scene before going on.\n"
    "- Before telling the user to do something by hand or that MiniMe cannot do it, check this list: most edits have a tool.\n"
    "- render_view leaves MiniMe's popups out; zoom_box_on_plate_mm looks closely at one area.\n"
    "\n"
    "WHAT YOU CANNOT DO (say so plainly; suggest the user's tool or the modeling program)\n"
    "- Angled, curved or shaped cuts and cut connectors (the user's Cut tool does them).\n"
    "- Brush-painting by hand, or making painted lines thicker, thinner or even: paint_color works on whole triangles by rule.\n"
    "- Turning painted areas or raised details into separate parts by their shape (only flat cuts and outline slabs).\n"
    "- Editing the mesh itself beyond adding / subtracting / copying / mirroring parts (no smoothing, sculpting, writing text).\n"
    "- Printing, sending to a printer, or opening and saving project files.";

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
                json changed = json::array();
                try {
                    changed = on_gui([]() -> json { return user_changes(); });
                } catch (...) {
                }
                struct Seen
                {
                    ~Seen()
                    {
                        try {
                            on_gui([]() -> json { mark_seen(); return {}; });
                        } catch (...) {
                        }
                    }
                } seen;
                try {
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
                if (!changed.empty()) {
                    std::string list;
                    for (const json &c : changed)
                        list += (list.empty() ? "" : ", ") + c.get<std::string>();
                    result["content"].push_back({{"type", "text"},
                                                 {"text", "Note: the user changed things in MiniMe since your last call (" + list +
                                                              "). Earlier numbers and indices may be out of date: look again (get_scene) before relying on them."}});
                }
            } catch (const std::exception &e) {
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
