// SliceOfMe: painting on the plate where tree supports may not (or may only) start, and the support base brush
// that lets other objects' supports rest on this object. The plate zones are stored in the plate's config
// (support_plate_block_area / support_plate_allow_area) and read by the tree support generators.

#include "GLGizmoFdmSupports.hpp"

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Support/SupportSurroundings.hpp"
#include "libslic3r/Tesselate.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/CameraUtils.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>
#include <imgui/imgui.h>

#include <boost/functional/hash.hpp>

#include <algorithm>
#include <cmath>

namespace Slic3r::GUI {

namespace {

const char *PLATE_KEYS[3] = {"support_plate_block_area", "support_plate_allow_area", "support_plate_land_area"};

const std::vector<Vec2ds> &plate_option(const PartPlate *plate, int idx)
{
    static const std::vector<Vec2ds> empty;
    const auto *opt = plate != nullptr ? const_cast<PartPlate *>(plate)->config()->option<ConfigOptionPointsGroups>(PLATE_KEYS[idx]) : nullptr;
    return opt != nullptr ? opt->values : empty;
}

// fills the cells of 'value' covered by 'areas' (scaled plate coordinates), scanline by scanline
void rasterize(std::vector<uint8_t> &cells, int w, int h, const Vec2d &origin, double cell, const ExPolygons &areas, uint8_t value)
{
    Lines edges;
    for (const ExPolygon &expoly : areas)
        for (const Polygon *poly : {&expoly.contour})
            append(edges, poly->lines());
    for (const ExPolygon &expoly : areas)
        for (const Polygon &hole : expoly.holes)
            append(edges, hole.lines());
    std::vector<double> xs;
    for (int j = 0; j < h; ++j) {
        const double y = origin.y() + (j + 0.5) * cell;
        const coord_t ys = coord_t(scale_(y));
        xs.clear();
        for (const Line &l : edges) {
            const Point &a = l.a, &b = l.b;
            if ((a.y() <= ys) == (b.y() <= ys))
                continue; // does not cross this row
            const double t = double(ys - a.y()) / double(b.y() - a.y());
            xs.push_back(unscale<double>(a.x()) + t * unscale<double>(b.x() - a.x()));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int i0 = std::max(0, int(std::ceil((xs[k] - origin.x()) / cell - 0.5)));
            const int i1 = std::min(w - 1, int(std::floor((xs[k + 1] - origin.x()) / cell - 0.5)));
            for (int i = i0; i <= i1; ++i)
                cells[size_t(j) * w + i] = value;
        }
    }
}

// runs of cells holding 'value' as rectangles, merged (scaled plate coordinates)
ExPolygons cells_to_areas(const std::vector<uint8_t> &cells, int w, int h, const Vec2d &origin, double cell, uint8_t value)
{
    Polygons rects;
    for (int j = 0; j < h; ++j) {
        int i = 0;
        while (i < w) {
            if (cells[size_t(j) * w + i] != value) {
                ++i;
                continue;
            }
            const int i0 = i;
            while (i < w && cells[size_t(j) * w + i] == value)
                ++i;
            const double x0 = origin.x() + i0 * cell, x1 = origin.x() + i * cell;
            const double y0 = origin.y() + j * cell, y1 = y0 + cell;
            Polygon rect;
            rect.points = {Point(coord_t(scale_(x0)), coord_t(scale_(y0))), Point(coord_t(scale_(x1)), coord_t(scale_(y0))),
                           Point(coord_t(scale_(x1)), coord_t(scale_(y1))), Point(coord_t(scale_(x0)), coord_t(scale_(y1)))};
            rects.emplace_back(std::move(rect));
        }
    }
    if (rects.empty())
        return {};
    // a slight grow / shrink closes the seams between the rows
    return expolygons_simplify(offset_ex(union_ex(rects), scale_(0.01)), scale_(0.1));
}

// a flat model of the areas (scaled plate coordinates), lying at z = 0
void init_flat_model(GLModel &model, const ExPolygons &areas)
{
    model.reset();
    if (areas.empty())
        return;
    const std::vector<Vec3d> triangles = triangulate_expolygons_3d(areas);
    GLModel::Geometry        geo;
    geo.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    geo.reserve_vertices(triangles.size());
    geo.reserve_indices(triangles.size());
    for (size_t k = 0; k + 2 < triangles.size(); k += 3) {
        for (int c = 0; c < 3; ++c)
            geo.add_vertex(Vec3f(triangles[k + c].cast<float>()), Vec3f(Vec3f::UnitZ()));
        geo.add_triangle(unsigned(k), unsigned(k + 1), unsigned(k + 2));
    }
    model.init_from(std::move(geo));
}

// the overhang slope below which supports are generated, as the minimum downward component of a face normal
double overhang_normal_z(const ModelObject &object)
{
    int angle = 30;
    if (const auto *opt = wxGetApp().preset_bundle->prints.get_edited_preset().config.option<ConfigOptionInt>("support_threshold_angle"))
        angle = opt->value;
    if (const ConfigOption *opt = object.config.option("support_threshold_angle"))
        angle = opt->getInt();
    if (angle <= 0)
        angle = 30;
    return std::cos(Geometry::deg2rad(double(std::clamp(angle, 1, 89))));
}

} // namespace

void GLGizmoFdmSupports::plate_show_scene(bool show)
{
    const ModelObject *mo   = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const int          inst = m_c->selection_info() ? m_c->selection_info()->get_active_instance() : -1;
    if (show) {
        m_parent.toggle_model_objects_visibility(true);
        // the edited object keeps the neutral colour of the painting tools
        if (mo != nullptr && inst != -1)
            m_parent.toggle_model_objects_visibility(true, mo, inst);
    } else if (mo != nullptr && inst != -1) {
        // free what only the plate tools use (a big model has many triangles)
        m_scene_casters.clear();
        m_up_triangles.clear();
        m_up_triangles.shrink_to_fit();
        m_shadow_key = 0;
        // back to the painting tools' view: only the edited object
        m_parent.toggle_model_objects_visibility(false);
        m_parent.toggle_model_objects_visibility(true, mo, inst);
    }
    m_parent.set_as_dirty();
}

void GLGizmoFdmSupports::plate_update_guides() const
{
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    PartPlate     *plate  = plates.get_curr_plate();
    if (plate == nullptr || m_plate.cells.empty())
        return;
    const Vec2d  plate_origin(plate->get_origin().x(), plate->get_origin().y());
    const Model &model = wxGetApp().model();

    // 1) under the overhangs of every object on this plate
    size_t key = 0;
    boost::hash_combine(key, m_plate.w);
    boost::hash_combine(key, m_plate.h);
    boost::hash_combine(key, plate_origin.x());
    boost::hash_combine(key, plate_origin.y());
    for (int obj_idx = 0; obj_idx < int(model.objects.size()); ++obj_idx) {
        const ModelObject *mo = model.objects[obj_idx];
        boost::hash_combine(key, overhang_normal_z(*mo));
        for (int inst_idx = 0; inst_idx < int(mo->instances.size()); ++inst_idx) {
            if (!plate->contain_instance(obj_idx, inst_idx))
                continue;
            boost::hash_combine(key, mo->id().id);
            boost::hash_combine(key, inst_idx);
            const Transform3d &ti = mo->instances[inst_idx]->get_matrix();
            for (int k = 0; k < 16; ++k)
                boost::hash_combine(key, ti.data()[k]);
            for (const ModelVolume *mv : mo->volumes) {
                boost::hash_combine(key, mv->id().id);
                boost::hash_combine(key, (const void *) &mv->mesh());
                boost::hash_combine(key, mv->mesh().its.indices.size());
                const Transform3d &tv = mv->get_matrix();
                for (int k = 0; k < 16; ++k)
                    boost::hash_combine(key, tv.data()[k]);
            }
        }
    }
    if (key != m_shadow_key || m_shadow_cells.size() != m_plate.cells.size()) {
        m_shadow_key = key;
        m_shadow_cells.assign(m_plate.cells.size(), 0);
        m_top_z.assign(m_plate.cells.size(), 0.f);
        m_up_triangles.clear();
        m_plate_models_dirty = true; // the "stand on objects" areas are drawn on the tops
        m_land_surface_dirty = true;
        m_land_prune_pending = true;
        const double cell = m_plate.cell;
        // the overhangs' shadow and, for every triangle, the height of the top surface
        bool         overhang = false;
        auto         mark     = [&](int i, int j, double z) {
            if (i >= 0 && j >= 0 && i < m_plate.w && j < m_plate.h) {
                const size_t idx = size_t(j) * m_plate.w + i;
                if (overhang)
                    m_shadow_cells[idx] = 1;
                m_top_z[idx] = std::max(m_top_z[idx], float(z));
            }
        };
        for (int obj_idx = 0; obj_idx < int(model.objects.size()); ++obj_idx) {
            const ModelObject *mo     = model.objects[obj_idx];
            const double       min_nz = overhang_normal_z(*mo);
            for (int inst_idx = 0; inst_idx < int(mo->instances.size()); ++inst_idx) {
                if (!plate->contain_instance(obj_idx, inst_idx))
                    continue;
                for (const ModelVolume *mv : mo->volumes) {
                    if (!mv->is_model_part())
                        continue;
                    const Transform3d          trafo = mo->instances[inst_idx]->get_matrix() * mv->get_matrix();
                    const indexed_triangle_set &its  = mv->mesh().its;
                    std::vector<Vec3d>          pts(its.vertices.size());
                    for (size_t v = 0; v < its.vertices.size(); ++v) {
                        pts[v] = trafo * its.vertices[v].cast<double>();
                        pts[v].x() -= plate_origin.x() + m_plate.origin.x();
                        pts[v].y() -= plate_origin.y() + m_plate.origin.y();
                    }
                    for (const stl_triangle_vertex_indices &f : its.indices) {
                        const Vec3d &a = pts[f[0]], &b = pts[f[1]], &c = pts[f[2]];
                        const double top_z = std::max({a.z(), b.z(), c.z()});
                        if (top_z < 0.3)
                            continue; // resting on the plate
                        const Vec3d  n   = (b - a).cross(c - a);
                        const double len = n.norm();
                        if (len <= 0.)
                            continue;
                        overhang = -n.z() >= min_nz * len;
                        // a top surface only matters where it faces up; an overhang where it faces down
                        if (!overhang && n.z() <= 0.)
                            continue;
                        if (!overhang && n.z() > 0.2 * len) {
                            // a surface supports could stand on: kept to draw "stand on objects" on it
                            const Vec3d centre = (a + b + c) / 3.;
                            const int   ci = int(std::floor(centre.x() / cell)), cj = int(std::floor(centre.y() / cell));
                            if (ci >= 0 && cj >= 0 && ci < m_plate.w && cj < m_plate.h) {
                                const Vec3f off(float(m_plate.origin.x()), float(m_plate.origin.y()), 0.f);
                                m_up_triangles.push_back({a.cast<float>() + off, b.cast<float>() + off, c.cast<float>() + off,
                                                          uint32_t(cj * m_plate.w + ci)});
                            }
                        }
                        const double minx = std::min({a.x(), b.x(), c.x()}), maxx = std::max({a.x(), b.x(), c.x()});
                        const double miny = std::min({a.y(), b.y(), c.y()}), maxy = std::max({a.y(), b.y(), c.y()});
                        const int    i0 = int(std::floor(minx / cell)), i1 = int(std::floor(maxx / cell));
                        const int    j0 = int(std::floor(miny / cell)), j1 = int(std::floor(maxy / cell));
                        if (i0 == i1 && j0 == j1) {
                            mark(i0, j0, overhang ? 0. : top_z); // small triangle: its cell
                            continue;
                        }
                        // the cells whose centre is inside the triangle, and the cells of its corners
                        const double area2 = (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
                        for (int j = std::max(0, j0); j <= std::min(m_plate.h - 1, j1); ++j)
                            for (int i = std::max(0, i0); i <= std::min(m_plate.w - 1, i1); ++i) {
                                const double px = (i + 0.5) * cell, py = (j + 0.5) * cell;
                                const double w0 = (b.x() - a.x()) * (py - a.y()) - (b.y() - a.y()) * (px - a.x());
                                const double w1 = (c.x() - b.x()) * (py - b.y()) - (c.y() - b.y()) * (px - b.x());
                                const double w2 = (a.x() - c.x()) * (py - c.y()) - (a.y() - c.y()) * (px - c.x());
                                if (area2 > 0. ? (w0 >= 0. && w1 >= 0. && w2 >= 0.) : (w0 <= 0. && w1 <= 0. && w2 <= 0.)) {
                                    // height of the triangle at the cell centre
                                    const double z = overhang || std::abs(area2) < 1e-12 ? 0. :
                                                                                           (w1 * a.z() + w2 * b.z() + w0 * c.z()) / area2;
                                    mark(i, j, z);
                                }
                            }
                        for (const Vec3d *p : {&a, &b, &c})
                            mark(int(std::floor(p->x() / cell)), int(std::floor(p->y() / cell)), overhang ? 0. : p->z());
                    }
                }
            }
        }
        size_t count = 0;
        for (uint8_t c : m_shadow_cells)
            count += c;
        m_shadow_area = double(count) * cell * cell;
        init_flat_model(m_shadow_model, cells_to_areas(m_shadow_cells, m_plate.w, m_plate.h, m_plate.origin, cell, 1));
    }

    // 2) the supports of the last slice of this plate: where they start (orange) and their shape (see-through walls).
    // While the plate is being changed (supports not calculated), the previous result stays, marked as old.
    const Print *print = plate->fff_print();
    std::vector<const PrintObject *> done;
    if (print != nullptr)
        for (const PrintObject *po : print->objects())
            if (po->is_step_done(posSupportMaterial) && po->is_step_done(posSimplifySupportPath) && !po->support_layers().empty())
                done.emplace_back(po);
    m_supports_old = done.empty() && (m_support_base_model.is_initialized() || m_support_walls_model.is_initialized());
    if (done.empty())
        return;
    size_t base_key = 0;
    boost::hash_combine(base_key, plate_origin.x());
    boost::hash_combine(base_key, plate_origin.y());
    for (const PrintObject *po : done) {
        boost::hash_combine(base_key, (const void *) po);
        boost::hash_combine(base_key, (const void *) po->support_layers().front());
        boost::hash_combine(base_key, po->support_layers().size());
        boost::hash_combine(base_key, po->step_state_with_timestamp(posSupportMaterial).timestamp);
    }
    if (base_key == m_support_base_key)
        return;
    m_support_base_key = base_key;

    const Point plate_shift(coord_t(scale_(plate_origin.x())), coord_t(scale_(plate_origin.y())));
    ExPolygons  base;
    GLModel::Geometry walls;
    walls.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    unsigned int vertices = 0;
    auto add_walls = [&](const ExPolygons &islands, const Point &shift, float z0, float z1) {
        for (const ExPolygon &expoly : islands)
            for (size_t ip = 0; ip <= expoly.holes.size(); ++ip) {
                const Points &pts = ip == 0 ? expoly.contour.points : expoly.holes[ip - 1].points;
                for (size_t k = 0; k < pts.size(); ++k) {
                    const Point  p  = pts[k] + shift, q = pts[(k + 1) % pts.size()] + shift;
                    const Vec2f  a  = unscale(p).cast<float>(), b = unscale(q).cast<float>();
                    const Vec2f  d  = b - a;
                    const float  len = d.norm();
                    if (len < 1e-4f)
                        continue;
                    const Vec3f  n(d.y() / len, -d.x() / len, 0.f);
                    walls.add_vertex(Vec3f(a.x(), a.y(), z0), n);
                    walls.add_vertex(Vec3f(b.x(), b.y(), z0), n);
                    walls.add_vertex(Vec3f(b.x(), b.y(), z1), n);
                    walls.add_vertex(Vec3f(a.x(), a.y(), z1), n);
                    walls.add_triangle(vertices, vertices + 1, vertices + 2);
                    walls.add_triangle(vertices, vertices + 2, vertices + 3);
                    vertices += 4;
                }
            }
    };
    for (const PrintObject *po : done) {
        // simplified, in slabs of about 0.6 mm: enough to see the branches, light enough for a big model
        std::vector<std::pair<std::pair<float, float>, ExPolygons>> slabs;
        float slab_bottom = -1.f;
        for (const SupportLayer *layer : po->support_layers()) {
            if (layer->support_islands.empty())
                continue;
            if (slab_bottom < 0.f || float(layer->print_z) - slab_bottom >= 0.6f) {
                slab_bottom = float(layer->print_z - layer->height);
                slabs.push_back({{slab_bottom, float(layer->print_z)}, expolygons_simplify(layer->support_islands, scale_(0.25))});
            }
            else
                slabs.back().first.second = float(layer->print_z);
        }
        for (const PrintInstance &pi : po->instances()) {
            const Point shift = pi.shift - plate_shift;
            ExPolygons  first = po->support_layers().front()->support_islands;
            for (ExPolygon &expoly : first)
                expoly.translate(shift);
            append(base, std::move(first));
            for (const auto &slab : slabs)
                add_walls(slab.second, shift, slab.first.first, slab.first.second);
        }
    }
    m_support_base_area = 0.;
    base                = union_ex(base);
    for (const ExPolygon &expoly : base)
        m_support_base_area += unscale<double>(unscale<double>(expoly.area()));
    init_flat_model(m_support_base_model, base);
    m_support_walls_model.reset();
    if (vertices > 0)
        m_support_walls_model.init_from(std::move(walls));
}

EnforcerBlockerType GLGizmoFdmSupports::get_left_button_state_type() const
{
    return m_paint_target == PaintTarget::SupportBase ? SUPPORT_LANDING_STATE : EnforcerBlockerType::ENFORCER;
}

EnforcerBlockerType GLGizmoFdmSupports::get_right_button_state_type() const
{
    // the support base brush: the right button erases it
    return m_paint_target == PaintTarget::SupportBase ? EnforcerBlockerType::NONE : EnforcerBlockerType::BLOCKER;
}

void GLGizmoFdmSupports::plate_sync_from_config()
{
    PartPlateList &plates = wxGetApp().plater()->get_partplate_list();
    PartPlate     *plate  = plates.get_curr_plate();
    if (plate == nullptr)
        return;
    const int index = plates.get_curr_plate_index();
    bool same = index == m_plate_index && !m_plate.cells.empty() && !m_plate_stroke;
    for (int idx = 0; same && idx < PLATE_ZONES; ++idx)
        same = plate_option(plate, idx) == m_plate_loaded[idx];
    if (same)
        return; // up to date (an undo / redo or another plate changes it)
    if (m_plate_stroke)
        return;

    // the grid covers the printable area (plate coordinates) and a margin around it
    BoundingBoxf box;
    if (const auto *area = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionPoints>("printable_area"))
        for (const Vec2d &pt : area->values)
            box.merge(pt);
    if (!box.defined)
        box = BoundingBoxf(Vec2d(0., 0.), Vec2d(256., 256.));
    m_plate.cell   = 0.5;
    m_plate.origin = box.min - Vec2d(5., 5.);
    m_plate.w      = int(std::ceil((box.size().x() + 10.) / m_plate.cell));
    m_plate.h      = int(std::ceil((box.size().y() + 10.) / m_plate.cell));
    m_plate.cells.assign(size_t(m_plate.w) * m_plate.h, 0);
    for (int idx = 0; idx < PLATE_ZONES; ++idx) {
        m_plate_loaded[idx] = plate_option(plate, idx);
        Polygons polys;
        for (const Vec2ds &pts : m_plate_loaded[idx]) {
            if (pts.size() < 3)
                continue;
            Polygon poly;
            for (const Vec2d &pt : pts)
                poly.points.emplace_back(coord_t(scale_(pt.x())), coord_t(scale_(pt.y())));
            polys.emplace_back(std::move(poly));
        }
        if (!polys.empty())
            rasterize(m_plate.cells, m_plate.w, m_plate.h, m_plate.origin, m_plate.cell, union_ex(polys, ClipperLib::pftNonZero), uint8_t(idx + 1));
    }
    m_plate_index        = index;
    m_plate_models_dirty = true;
}

ExPolygons GLGizmoFdmSupports::plate_areas(uint8_t value) const
{
    return cells_to_areas(m_plate.cells, m_plate.w, m_plate.h, m_plate.origin, m_plate.cell, value);
}

void GLGizmoFdmSupports::plate_commit()
{
    PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return;
    std::vector<Vec2ds> values[PLATE_ZONES];
    for (int idx = 0; idx < PLATE_ZONES; ++idx)
        for (const ExPolygon &expoly : plate_areas(uint8_t(idx + 1))) {
            // contours counter-clockwise, holes clockwise: read back with non-zero filling
            for (const Polygon *poly : {&expoly.contour}) {
                Vec2ds pts;
                for (const Point &pt : poly->points)
                    pts.emplace_back(unscale<double>(pt.x()), unscale<double>(pt.y()));
                values[idx].emplace_back(std::move(pts));
            }
            for (const Polygon &hole : expoly.holes) {
                Vec2ds pts;
                for (const Point &pt : hole.points)
                    pts.emplace_back(unscale<double>(pt.x()), unscale<double>(pt.y()));
                values[idx].emplace_back(std::move(pts));
            }
        }
    bool unchanged = true;
    for (int idx = 0; unchanged && idx < PLATE_ZONES; ++idx)
        unchanged = values[idx] == plate_option(plate, idx);
    if (unchanged)
        return;
    {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Paint support zones on the plate", UndoRedo::SnapshotType::GizmoAction);
        for (int idx = 0; idx < PLATE_ZONES; ++idx) {
            if (values[idx].empty())
                plate->config()->erase(PLATE_KEYS[idx]);
            else
                plate->config()->set_key_value(PLATE_KEYS[idx], new ConfigOptionPointsGroups(values[idx]));
            m_plate_loaded[idx] = plate_option(plate, idx);
        }
    }
    wxGetApp().plater()->set_plater_dirty(true);
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
}

bool GLGizmoFdmSupports::plate_point_on_objects(const Vec2d &mouse_position, Vec2d &plate_pt) const
{
    PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return false;
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Model  &model  = wxGetApp().model();
    double        best   = std::numeric_limits<double>::max();
    bool          found  = false;
    for (int obj_idx = 0; obj_idx < int(model.objects.size()); ++obj_idx) {
        const ModelObject *mo = model.objects[obj_idx];
        for (int inst_idx = 0; inst_idx < int(mo->instances.size()); ++inst_idx) {
            if (!plate->contain_instance(obj_idx, inst_idx))
                continue;
            for (const ModelVolume *mv : mo->volumes) {
                if (!mv->is_model_part())
                    continue;
                const std::shared_ptr<const TriangleMesh> &mesh = mv->get_mesh_shared_ptr();
                if (!mesh)
                    continue;
                auto it = std::find_if(m_scene_casters.begin(), m_scene_casters.end(), [&mesh](const auto &c) { return c.first == mesh; });
                if (it == m_scene_casters.end()) {
                    m_scene_casters.emplace_back(mesh, std::make_unique<MeshRaycaster>(*mesh));
                    it = std::prev(m_scene_casters.end());
                }
                const Transform3d trafo = mo->instances[inst_idx]->get_matrix() * mv->get_matrix();
                Vec3f             pos, normal;
                if (!it->second->unproject_on_mesh(mouse_position, trafo, camera, pos, normal, nullptr, nullptr, false))
                    continue;
                const Vec3d  world = trafo * pos.cast<double>();
                const double dist  = (world - camera.get_position()).norm();
                if (dist < best) {
                    best          = dist;
                    m_last_hit_z  = world.z();
                    plate_pt = Vec2d(world.x(), world.y()) - Vec2d(plate->get_origin().x(), plate->get_origin().y());
                    found    = true;
                }
            }
        }
    }
    if (!found)
        return false;
    const Vec2d rel = plate_pt - m_plate.origin;
    return rel.x() >= 0. && rel.y() >= 0. && rel.x() < m_plate.w * m_plate.cell && rel.y() < m_plate.h * m_plate.cell;
}

bool GLGizmoFdmSupports::plate_point(const Vec2d &mouse_position, Vec2d &plate_pt) const
{
    const PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr)
        return false;
    // "stand on objects" is painted where the mouse is on an object
    if (m_paint_target == PaintTarget::PlateLand && plate_point_on_objects(mouse_position, plate_pt))
        return true;
    m_last_hit_z = 0.;
    Vec3d point, dir;
    CameraUtils::ray_from_screen_pos(wxGetApp().plater()->get_camera(), mouse_position, point, dir);
    if (std::abs(dir.z()) < 1e-9)
        return false;
    const double t = -point.z() / dir.z();
    if (t < 0.)
        return false;
    const Vec3d world = point + t * dir;
    plate_pt          = Vec2d(world.x(), world.y()) - Vec2d(plate->get_origin().x(), plate->get_origin().y());
    const Vec2d rel   = plate_pt - m_plate.origin;
    return rel.x() >= 0. && rel.y() >= 0. && rel.x() < m_plate.w * m_plate.cell && rel.y() < m_plate.h * m_plate.cell;
}

void GLGizmoFdmSupports::plate_stamp(const Vec2d &p, uint8_t value)
{
    const double r  = 0.5 * std::max(0.5, double(m_plate_brush_size));
    const int    i0 = std::max(0, int(std::floor((p.x() - r - m_plate.origin.x()) / m_plate.cell)));
    const int    i1 = std::min(m_plate.w - 1, int(std::ceil((p.x() + r - m_plate.origin.x()) / m_plate.cell)));
    const int    j0 = std::max(0, int(std::floor((p.y() - r - m_plate.origin.y()) / m_plate.cell)));
    const int    j1 = std::min(m_plate.h - 1, int(std::ceil((p.y() + r - m_plate.origin.y()) / m_plate.cell)));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const Vec2d c = m_plate.origin + Vec2d((i + 0.5) * m_plate.cell, (j + 0.5) * m_plate.cell);
            const Vec2d d = c - p;
            if (m_plate_brush == PlateBrush::Square ? (std::abs(d.x()) <= r && std::abs(d.y()) <= r) : d.squaredNorm() <= r * r) {
                const size_t idx = size_t(j) * m_plate.w + i;
                // "stand on objects" belongs to the objects: the brush does not spill onto the bare plate around them
                if (value == 3 && idx < m_top_z.size() && m_top_z[idx] <= 0.3f)
                    continue;
                m_plate.cells[idx] = value;
            }
        }
    m_plate_models_dirty = true;
}

void GLGizmoFdmSupports::plate_prune_land()
{
    m_land_prune_pending = false;
    if (m_top_z.size() != m_plate.cells.size())
        return;
    bool changed = false;
    for (size_t idx = 0; idx < m_plate.cells.size(); ++idx)
        if (m_plate.cells[idx] == 3 && m_top_z[idx] <= 0.3f) {
            m_plate.cells[idx] = 0;
            changed            = true;
        }
    if (changed) {
        m_plate_models_dirty = true;
        m_land_surface_dirty = true;
        plate_commit();
    }
}

void GLGizmoFdmSupports::plate_bucket(const Vec2d &p, uint8_t value)
{
    const int i = int(std::floor((p.x() - m_plate.origin.x()) / m_plate.cell));
    const int j = int(std::floor((p.y() - m_plate.origin.y()) / m_plate.cell));
    if (i < 0 || j < 0 || i >= m_plate.w || j >= m_plate.h)
        return;
    const size_t seed = size_t(j) * m_plate.w + i;
    if (m_paint_target == PaintTarget::PlateLand && seed < m_top_z.size() && m_top_z[seed] > 0.3f) {
        // on an object: the surface around the clicked point, spreading to neighbours whose height differs at most by
        // the tolerance (a flat top fills up to its edges, a curved one as far as its slope stays gentle)
        const float      tol = std::max(0.01f, m_bucket_tolerance);
        std::vector<uint8_t> seen(m_plate.cells.size(), 0);
        std::vector<int>     region;
        std::vector<int>     stack{int(seed)};
        seen[seed] = 1;
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            region.push_back(c);
            const int ci = c % m_plate.w, cj = c / m_plate.w;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int ni = ci + di, nj = cj + dj;
                    if ((di == 0 && dj == 0) || ni < 0 || nj < 0 || ni >= m_plate.w || nj >= m_plate.h)
                        continue;
                    const int n = nj * m_plate.w + ni;
                    if (seen[n] || m_top_z[n] <= 0.3f || std::abs(m_top_z[n] - m_top_z[c]) > tol)
                        continue;
                    seen[n] = 1;
                    stack.push_back(n);
                }
        }
        for (int c : region)
            m_plate.cells[c] = value;
        // close pin holes (spots of the surface no triangle centre fell into)
        std::vector<int> holes;
        for (int c : region) {
            const int ci = c % m_plate.w, cj = c / m_plate.w;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int ni = ci + di, nj = cj + dj;
                    if (ni < 1 || nj < 1 || ni >= m_plate.w - 1 || nj >= m_plate.h - 1)
                        continue;
                    const int n = nj * m_plate.w + ni;
                    if (seen[n])
                        continue;
                    int around = 0;
                    for (int ej = -1; ej <= 1; ++ej)
                        for (int ei = -1; ei <= 1; ++ei)
                            around += seen[(nj + ej) * m_plate.w + ni + ei];
                    if (around >= 5) {
                        seen[n] = 1;
                        holes.push_back(n);
                    }
                }
        }
        for (int c : holes)
            m_plate.cells[c] = value;
        m_plate_models_dirty = true;
        m_land_surface_dirty = true;
        return;
    }
    const uint8_t from = m_plate.cells[seed];
    if (from == value)
        return;
    // the whole area of the same kind around the clicked point: e.g. the inside of a painted outline
    std::vector<int> stack{j * m_plate.w + i};
    m_plate.cells[size_t(j) * m_plate.w + i] = value;
    while (!stack.empty()) {
        const int c = stack.back();
        stack.pop_back();
        const int ci = c % m_plate.w, cj = c / m_plate.w;
        const int nbs[4][2] = {{ci - 1, cj}, {ci + 1, cj}, {ci, cj - 1}, {ci, cj + 1}};
        for (const auto &nb : nbs)
            if (nb[0] >= 0 && nb[1] >= 0 && nb[0] < m_plate.w && nb[1] < m_plate.h) {
                uint8_t &cell = m_plate.cells[size_t(nb[1]) * m_plate.w + nb[0]];
                if (cell == from) {
                    cell = value;
                    stack.push_back(nb[1] * m_plate.w + nb[0]);
                }
            }
    }
    m_plate_models_dirty = true;
    m_land_surface_dirty = true;
}

bool GLGizmoFdmSupports::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (!plate_mode())
        return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);

    plate_sync_from_config();
    if (m_plate.cells.empty())
        return false;
    const bool wheel = action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown;
    if (wheel && (control_down || alt_down)) {
        // brush size
        m_plate_brush_size = std::clamp(m_plate_brush_size * (action == SLAGizmoEventType::MouseWheelUp ? 1.1f : 1.f / 1.1f), 1.f, 200.f);
        m_parent.set_as_dirty();
        return true;
    }
    if (control_down)
        return false; // Ctrl + drag turns the view
    Vec2d      p;
    const bool on_plate = plate_point(mouse_position, p);
    switch (action) {
    case SLAGizmoEventType::Moving:
        m_plate_hover = on_plate ? std::optional<Vec2d>(p) : std::nullopt;
        m_parent.set_as_dirty();
        return false;
    case SLAGizmoEventType::LeftDown:
    case SLAGizmoEventType::RightDown: {
        if (!on_plate)
            return false; // off the plate: turn the view
        // left paints, right (or Shift) erases
        const uint8_t value = (action == SLAGizmoEventType::RightDown || shift_down) ? 0 : (m_paint_target == PaintTarget::PlateBlock ? 1 : m_paint_target == PaintTarget::PlateAllow ? 2 : 3);
        if (m_plate_brush == PlateBrush::Bucket) {
            plate_bucket(p, value);
            plate_commit();
        } else {
            m_plate_stroke       = true;
            m_plate_stroke_value = value;
            m_plate_last         = p;
            plate_stamp(p, value);
        }
        m_plate_hover = p;
        m_parent.set_as_dirty();
        return true;
    }
    case SLAGizmoEventType::Dragging:
        if (!m_plate_stroke)
            return false;
        if (on_plate) {
            // fill the gap since the last mouse position
            const Vec2d  from  = m_plate_last ? *m_plate_last : p;
            const double step  = std::max(m_plate.cell, 0.25 * double(m_plate_brush_size));
            const int    steps = std::max(1, int(std::ceil((p - from).norm() / step)));
            for (int s = 1; s <= steps; ++s)
                plate_stamp(from + (p - from) * (double(s) / steps), m_plate_stroke_value);
            m_plate_last  = p;
            m_plate_hover = p;
            m_parent.set_as_dirty();
        }
        return true;
    case SLAGizmoEventType::LeftUp:
    case SLAGizmoEventType::RightUp:
        if (!m_plate_stroke)
            return false;
        m_plate_stroke = false;
        m_plate_last.reset();
        plate_commit();
        m_parent.set_as_dirty();
        return true;
    default: return false;
    }
}

void GLGizmoFdmSupports::render_plate_zones() const
{
    const PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (plate == nullptr || m_plate.cells.empty())
        return;
    if (plate_mode())
        plate_update_guides(); // also the heights of the tops, for "stand on objects"
    if (m_plate_models_dirty) {
        m_plate_models_dirty = false;
        // the painted areas as seen from above, on the plate ("stand on objects" faint: the surfaces show it)
        for (int idx = 0; idx < PLATE_ZONES; ++idx)
            init_flat_model(m_plate_models[idx], plate_areas(uint8_t(idx + 1)));
        m_land_surface_dirty = true;
    }
    // "stand on objects" on the objects' real surfaces; while painting at most a few times per second (big models)
    if (m_land_surface_dirty &&
        (!m_plate_stroke || std::chrono::steady_clock::now() - m_land_surface_built > std::chrono::milliseconds(150))) {
        m_land_surface_dirty = false;
        m_land_surface_built = std::chrono::steady_clock::now();
        m_land_surface_model.reset();
        GLModel::Geometry geo;
        geo.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        unsigned int vertices = 0;
        const Vec3f  lift(0.f, 0.f, 0.03f);
        for (const UpTriangle &t : m_up_triangles) {
            if (t.cell >= m_plate.cells.size() || m_plate.cells[t.cell] != 3)
                continue;
            geo.add_vertex(Vec3f(t.a + lift), Vec3f(Vec3f::UnitZ()));
            geo.add_vertex(Vec3f(t.b + lift), Vec3f(Vec3f::UnitZ()));
            geo.add_vertex(Vec3f(t.c + lift), Vec3f(Vec3f::UnitZ()));
            geo.add_triangle(vertices, vertices + 1, vertices + 2);
            vertices += 3;
        }
        if (vertices > 0)
            m_land_surface_model.init_from(std::move(geo));
    }
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d plate_trafo = Geometry::assemble_transform(Vec3d(plate->get_origin().x(), plate->get_origin().y(), 0.05));
    const float       alpha       = plate_mode() ? 0.55f : 0.3f;
    const std::array<float, 4> colors[PLATE_ZONES] = {{0.9f, 0.2f, 0.2f, alpha}, {0.2f, 0.8f, 0.3f, alpha}, {0.95f, 0.85f, 0.1f, alpha}};
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    if (plate_mode()) {
        // under the overhangs (blue) and where the supports started in the last slice (orange), below the zones
        const Transform3d shadow_trafo = Geometry::assemble_transform(Vec3d(plate->get_origin().x(), plate->get_origin().y(), 0.02));
        const Transform3d base_trafo   = Geometry::assemble_transform(Vec3d(plate->get_origin().x(), plate->get_origin().y(), 0.035));
        if (m_shadow_model.is_initialized())
            render_glmodel(m_shadow_model, {0.25f, 0.55f, 0.95f, 0.35f}, camera.get_view_matrix() * shadow_trafo, camera.get_projection_matrix());
        if (m_support_base_model.is_initialized())
            render_glmodel(m_support_base_model, {1.0f, 0.55f, 0.1f, 0.75f}, camera.get_view_matrix() * base_trafo, camera.get_projection_matrix());
    }
    for (int idx = 0; idx < PLATE_ZONES; ++idx)
        if (m_plate_models[idx].is_initialized()) {
            std::array<float, 4> color = colors[idx];
            if (idx == 2)
                color[3] *= 0.4f; // the footprint of "stand on objects": the surfaces below show where it applies
            render_glmodel(m_plate_models[idx], color, camera.get_view_matrix() * plate_trafo, camera.get_projection_matrix());
        }
    // the object surfaces supports may stand on
    if (m_land_surface_model.is_initialized()) {
        const Transform3d surface_trafo = Geometry::assemble_transform(Vec3d(plate->get_origin().x(), plate->get_origin().y(), 0.));
        render_glmodel(m_land_surface_model, {1.f, 0.85f, 0.05f, plate_mode() ? 0.9f : 0.5f}, camera.get_view_matrix() * surface_trafo,
                       camera.get_projection_matrix());
    }

    // the supports of the last slice, see-through (grey when the plate changed since)
    if (plate_mode() && m_show_supports && m_support_walls_model.is_initialized()) {
        const Transform3d walls_trafo = Geometry::assemble_transform(Vec3d(plate->get_origin().x(), plate->get_origin().y(), 0.));
        glsafe(::glDepthMask(GL_FALSE));
        render_glmodel(m_support_walls_model, m_supports_old ? std::array<float, 4>{0.6f, 0.6f, 0.6f, 0.25f} : std::array<float, 4>{0.1f, 0.85f, 0.35f, 0.3f},
                       camera.get_view_matrix() * walls_trafo, camera.get_projection_matrix());
        glsafe(::glDepthMask(GL_TRUE));
    }

    // the brush under the mouse
    if (plate_mode() && m_plate_hover && m_plate_brush != PlateBrush::Bucket) {
        const std::array<float, 2> key{float(m_plate_brush), m_plate_brush_size};
        if (key != m_plate_cursor_key || !m_plate_cursor_model.is_initialized()) {
            m_plate_cursor_key = key;
            const double r     = 0.5 * double(m_plate_brush_size);
            Polygon      shape;
            if (m_plate_brush == PlateBrush::Square)
                shape.points = {Point(coord_t(scale_(-r)), coord_t(scale_(-r))), Point(coord_t(scale_(r)), coord_t(scale_(-r))),
                                Point(coord_t(scale_(r)), coord_t(scale_(r))), Point(coord_t(scale_(-r)), coord_t(scale_(r)))};
            else
                for (int k = 0; k < 48; ++k)
                    shape.points.emplace_back(coord_t(scale_(r * std::cos(2. * M_PI * k / 48))), coord_t(scale_(r * std::sin(2. * M_PI * k / 48))));
            const std::vector<Vec3d> triangles = triangulate_expolygons_3d({ExPolygon(shape)});
            GLModel::Geometry        geo;
            geo.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
            for (size_t k = 0; k + 2 < triangles.size(); k += 3) {
                for (int c = 0; c < 3; ++c)
                    geo.add_vertex(Vec3f(triangles[k + c].cast<float>()), Vec3f(Vec3f::UnitZ()));
                geo.add_triangle(unsigned(k), unsigned(k + 1), unsigned(k + 2));
            }
            m_plate_cursor_model.reset();
            m_plate_cursor_model.init_from(std::move(geo));
        }
        const Transform3d cursor_trafo = Geometry::assemble_transform(
            Vec3d(plate->get_origin().x() + m_plate_hover->x(), plate->get_origin().y() + m_plate_hover->y(), m_last_hit_z + 0.1));
        render_glmodel(m_plate_cursor_model, {1.f, 1.f, 1.f, 0.35f}, camera.get_view_matrix() * cursor_trafo, camera.get_projection_matrix());
    }
    glsafe(::glEnable(GL_CULL_FACE));
}

void GLGizmoFdmSupports::render_plate_panel(float label_width, float slider_width, float drag_left_width)
{
    plate_sync_from_config();
    if (m_land_prune_pending && !m_plate_stroke)
        plate_prune_land();
    if (m_paint_target == PaintTarget::PlateBlock) {
        m_imgui->text(_L("Paint where supports may not start on the plate."));
        m_imgui->text(_L("The branches move to another place."));
    } else if (m_paint_target == PaintTarget::PlateAllow) {
        m_imgui->text(_L("Paint where supports may start on the plate; nowhere else."));
    } else {
        m_imgui->text(_L("Paint over an object: supports that can reach it stand on it"));
        m_imgui->text(_L("instead of going to the plate, even when supports are limited"));
        m_imgui->text(_L("to the build plate. The paint stays on the objects: what is left"));
        m_imgui->text(_L("on the bare plate (object deleted or moved) is removed."));
    }
    m_imgui->text(_L("Applies to all tree supports on this plate (organic, hybrid and tree)."));
    ImGui::Separator();
    // what the colours on the plate mean
    auto legend = [this](const ImVec4 &color, const wxString &text) {
        ImGui::ColorButton(("##legend" + into_u8(text)).c_str(), color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                           ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        m_imgui->text(text);
    };
    legend(ImVec4(0.25f, 0.55f, 0.95f, 1.f), wxString::Format(_L("Under overhangs: where supports would start (%.0f mm²)"), m_shadow_area));
    if (m_support_base_area > 0.)
        legend(ImVec4(1.0f, 0.55f, 0.1f, 1.f), wxString::Format(_L("Supports start here in the last slice (%.0f mm²)"), m_support_base_area));
    else
        legend(ImVec4(1.0f, 0.55f, 0.1f, 1.f), _L("Slice the plate to see where the supports start now"));
    // the supports themselves, in 3D
    m_imgui->bbl_checkbox(_L("Show the supports of the last slice"), m_show_supports);
    if (m_supports_old)
        m_imgui->text(_L("Changed since the last slice: the supports shown (grey) are the old ones."));
    if (m_imgui->button(_L("Slice plate to update the supports")))
        // after this frame: slicing may show a dialog
        wxGetApp().CallAfter([]() { wxGetApp().plater()->reslice(); });
    legend(ImVec4(0.9f, 0.2f, 0.2f, 1.f), _L("No supports here"));
    legend(ImVec4(0.2f, 0.8f, 0.3f, 1.f), _L("Supports only here"));
    legend(ImVec4(0.95f, 0.85f, 0.1f, 1.f), _L("Supports stand on the object here"));
    ImGui::Separator();
    m_imgui->text(_L("Brush"));
    ImGui::SameLine(label_width);
    if (ImGui::RadioButton((_u8L("Circle") + "##plate_brush").c_str(), m_plate_brush == PlateBrush::Circle))
        m_plate_brush = PlateBrush::Circle;
    ImGui::SameLine();
    if (ImGui::RadioButton((_u8L("Square") + "##plate_brush").c_str(), m_plate_brush == PlateBrush::Square))
        m_plate_brush = PlateBrush::Square;
    ImGui::SameLine();
    if (ImGui::RadioButton((_u8L("Bucket") + "##plate_brush").c_str(), m_plate_brush == PlateBrush::Bucket))
        m_plate_brush = PlateBrush::Bucket;
    if (m_plate_brush != PlateBrush::Bucket) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Size") + " (mm)");
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(slider_width);
        m_imgui->bbl_slider_float_style("##plate_brush_size", &m_plate_brush_size, 1.f, 200.f, "%.1f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(m_imgui->scaled(3.5f));
        if (ImGui::BBLDragFloat("##plate_brush_size_input", &m_plate_brush_size, 0.1f, 0.0f, 0.0f, "%.1f"))
            m_plate_brush_size = std::clamp(m_plate_brush_size, 1.f, 200.f);
    } else if (m_paint_target == PaintTarget::PlateLand) {
        // the bucket picks a surface of an object
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Tolerance") + " (mm)");
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(slider_width);
        m_imgui->bbl_slider_float_style("##bucket_tolerance", &m_bucket_tolerance, 0.1f, 10.f, "%.1f", 1.0f, true);
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(m_imgui->scaled(3.5f));
        if (ImGui::BBLDragFloat("##bucket_tolerance_input", &m_bucket_tolerance, 0.1f, 0.0f, 0.0f, "%.1f"))
            m_bucket_tolerance = std::clamp(m_bucket_tolerance, 0.1f, 10.f);
        m_imgui->text(_L("Click a surface of an object: it fills while the height between"));
        m_imgui->text(_L("neighbouring spots changes less than the tolerance."));
        m_imgui->text(_L("Small: one flat face. Large: the whole top of a curved part."));
    } else
        m_imgui->text(_L("Click inside an outline to fill it, or on a painted area to recolour it all."));
    m_imgui->text(_L("Left mouse: paint.  Right mouse / Shift: erase.  Ctrl + wheel: size."));
    ImGui::Separator();
    // what is there now
    int blocked = 0, allowed = 0, standing = 0;
    for (uint8_t c : m_plate.cells) {
        blocked += c == 1;
        allowed += c == 2;
        standing += c == 3;
    }
    const double cell_area = m_plate.cell * m_plate.cell;
    m_imgui->text(wxString::Format(_L("Blocked: %.0f mm²   Allowed only: %.0f mm²   On objects: %.0f mm²"), blocked * cell_area,
                                   allowed * cell_area, standing * cell_area));
    if (allowed > 0)
        m_imgui->text(_L("Allowed areas exist: supports start only inside the green areas."));
    if (m_imgui->button(_L("Clear plate zones"))) {
        std::fill(m_plate.cells.begin(), m_plate.cells.end(), uint8_t(0));
        m_plate_models_dirty = true;
        plate_commit();
    }
}

} // namespace Slic3r::GUI
