// SliceOfMe: curved and shape cuts for the Cut gizmo.
//
// The outline drawn on the cut plane becomes a prism that goes through the whole object along the
// plane normal; mesh booleans split the object into part A (inside the prism) and part B (the rest).
//  - Curved: an open path drawn with a curvature pen. It is extended past the object and closed on its
//    left side, so it splits the object in two.
//  - Shape: a closed circle, ellipse, rectangle, rounded rectangle or polygon that works like a cookie cutter.
// The split is computed on a background thread and shown in the A/B colours. Each connected piece of the
// result can be moved to the other side with a right-click, and the cut uses exactly what is shown.

#include "GLGizmoAdvancedCut.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

#include <GL/glew.h>
#include <boost/log/trivial.hpp>

#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/Tesselate.hpp"

namespace Slic3r {
namespace GUI {

struct GLGizmoAdvancedCut::CurvePreview
{
    struct Piece
    {
        size_t               volume{0}; // index of the model volume it comes from
        bool                 in_a{true};
        indexed_triangle_set its;       // world coordinates
    };
    std::mutex         mutex;
    size_t             wanted{0};         // generation the gizmo is waiting for
    size_t             ready{size_t(-1)}; // generation of 'pieces'
    bool               failed{false};
    std::vector<Piece> pieces;
};

struct GLGizmoAdvancedCut::CurvePiece
{
    size_t                         volume{0};
    bool                           in_a{true};
    std::shared_ptr<TriangleMesh>  mesh;      // world coordinates
    std::shared_ptr<MeshRaycaster> raycaster; // references 'mesh'
    std::shared_ptr<GLModel>       model;
};

namespace {

// Same id as in GLGizmoAdvancedCut.cpp
constexpr int CURVE_PLATE_MOVE_ID = 5;

const ColorRGBA CURVE_PART_A_COLOR = ColorRGBA::CYAN();
const ColorRGBA CURVE_PART_B_COLOR = ColorRGBA::MAGENTA();

// Closed solid from 2D shapes (plane frame, mm): caps at z = -depth / +depth plus side walls.
indexed_triangle_set extrude_shapes(const ExPolygons &shapes, double depth)
{
    std::vector<Vec3d> soup;
    for (const ExPolygon &shape : shapes) {
        // already unscaled to mm by the tesselator
        std::vector<Vec3d> top    = triangulate_expolygon_3d(shape, depth, false);
        std::vector<Vec3d> bottom = triangulate_expolygon_3d(shape, -depth, true);
        soup.insert(soup.end(), top.begin(), top.end());
        soup.insert(soup.end(), bottom.begin(), bottom.end());
        // Contour is CCW and holes CW, so (a, b) with "up" gives outward facing walls for both.
        auto add_walls = [&](const Polygon &poly) {
            const size_t m = poly.points.size();
            for (size_t i = 0; i < m; ++i) {
                const Vec2d a = unscale(poly.points[i]);
                const Vec2d b = unscale(poly.points[(i + 1) % m]);
                const Vec3d a0(a.x(), a.y(), -depth), b0(b.x(), b.y(), -depth), a1(a.x(), a.y(), depth), b1(b.x(), b.y(), depth);
                soup.insert(soup.end(), {a0, b0, b1, a0, b1, a1});
            }
        };
        add_walls(shape.contour);
        for (const Polygon &hole : shape.holes)
            add_walls(hole);
    }
    indexed_triangle_set its;
    its.vertices.reserve(soup.size());
    for (size_t i = 0; i + 2 < soup.size(); i += 3) {
        const int base = int(its.vertices.size());
        its.vertices.emplace_back(soup[i].cast<float>());
        its.vertices.emplace_back(soup[i + 1].cast<float>());
        its.vertices.emplace_back(soup[i + 2].cast<float>());
        its.indices.emplace_back(base, base + 1, base + 2);
    }
    its_merge_vertices(its);
    its_remove_degenerate_faces(its);
    its_compactify_vertices(its);
    return its;
}

// A op B with the default backend, CGAL as fallback. 'ok' is set to false when both failed.
// 'cancel' (optional) is polled by the boolean backend; a cancelled run returns an empty mesh.
indexed_triangle_set mesh_boolean(const TriangleMesh &a, const TriangleMesh &b, bool intersect, bool &ok,
                                  const std::function<bool()> &cancel = nullptr)
{
    std::vector<TriangleMesh> results;
    bool failed = false;
    MeshBoolean::mcut::make_boolean(a, b, results, intersect ? "INTERSECTION" : "A_NOT_B", cancel, nullptr, [&failed]() { failed = true; });
    if (cancel && cancel())
        return {};
    indexed_triangle_set its;
    if (!results.empty()) {
        for (const TriangleMesh &r : results)
            its_merge(its, r.its);
    } else if (failed) {
        try {
            TriangleMesh tmp = a;
            if (intersect)
                MeshBoolean::cgal::intersect(tmp, b);
            else
                MeshBoolean::cgal::minus(tmp, b);
            its = tmp.its;
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Curve cut: mesh boolean failed: " << e.what();
            ok = false;
            return {};
        }
    }
    its_remove_degenerate_faces(its);
    its_merge_vertices(its);
    its_compactify_vertices(its);
    return its;
}

Vec2d bezier(const Vec2d &p0, const Vec2d &p1, const Vec2d &p2, const Vec2d &p3, double t)
{
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
}

Vec2d rotate2d(const Vec2d &v, double deg)
{
    const double a = deg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
    return Vec2d(c * v.x() - s * v.y(), s * v.x() + c * v.y());
}

// Closed outline of a shape, counter-clockwise, plane frame.
std::vector<Vec2d> shape_outline(const GLGizmoAdvancedCut::CurveShape &shape)
{
    using Type = GLGizmoAdvancedCut::ShapeType;
    std::vector<Vec2d> pts;
    if (!shape.valid())
        return pts;
    const double hw = 0.5 * shape.size.x(), hh = 0.5 * shape.size.y();
    switch (shape.type) {
    case Type::Circle:
    case Type::Ellipse:
        for (int i = 0; i < 96; ++i) {
            const double a = 2.0 * M_PI * i / 96;
            pts.emplace_back(hw * std::cos(a), hh * std::sin(a));
        }
        break;
    case Type::Rectangle: pts = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}}; break;
    case Type::RoundedRect: {
        const double r = std::clamp(shape.corner_radius, 0.0, std::min(hw, hh));
        const Vec2d  corners[4] = {{hw - r, -hh + r}, {hw - r, hh - r}, {-hw + r, hh - r}, {-hw + r, -hh + r}};
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i <= 12; ++i) {
                const double a = (c - 1) * 0.5 * M_PI + 0.5 * M_PI * i / 12;
                pts.push_back(corners[c] + r * Vec2d(std::cos(a), std::sin(a)));
            }
        break;
    }
    case Type::Polygon: {
        const int n = std::max(3, shape.sides);
        for (int i = 0; i < n; ++i) {
            const double a = 0.5 * M_PI + 2.0 * M_PI * i / n; // a vertex on top
            pts.emplace_back(hw * std::cos(a), hh * std::sin(a));
        }
        break;
    }
    }
    for (Vec2d &p : pts)
        p = shape.center + rotate2d(p, shape.rotation);
    return pts;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Geometry

Transform3d GLGizmoAdvancedCut::curve_plane_matrix() const
{
    return Geometry::translation_transform(m_plane_center) * m_rotate_matrix;
}

bool GLGizmoAdvancedCut::curve_unproject(const Vec2d &mouse_pos, Vec2d &local)
{
    Vec3d pos, pos_world;
    if (!unproject_on_cut_plane(mouse_pos, pos, pos_world, false))
        return false;
    const Vec3d p = curve_plane_matrix().inverse() * pos_world;
    local         = Vec2d(p.x(), p.y());
    return true;
}

std::vector<GLGizmoAdvancedCut::CurveAnchor> GLGizmoAdvancedCut::curve_effective_anchors(const std::vector<CurveAnchor> &anchors) const
{
    // Curvature pen: a smooth point gets Catmull-Rom style handles from its neighbours, so the path
    // runs smoothly through every point without the user touching any handle.
    std::vector<CurveAnchor> out = anchors;
    const size_t             n   = out.size();
    for (size_t i = 0; i < n; ++i) {
        CurveAnchor &a = out[i];
        if (a.manual)
            continue;
        if (a.corner || n < 3 || i == 0 || i + 1 == n) {
            a.h_in = a.h_out = Vec2d::Zero();
            continue;
        }
        // Direction from the neighbours, length from the nearer one: a long Catmull-Rom tangent next to
        // a short span overshoots and draws a loop.
        const Vec2d  dir = anchors[i + 1].p - anchors[i - 1].p;
        const double len = std::min((anchors[i].p - anchors[i - 1].p).norm(), (anchors[i + 1].p - anchors[i].p).norm()) / 3.0;
        const Vec2d  tangent = dir.norm() > EPSILON ? Vec2d(dir.normalized() * len) : Vec2d::Zero();
        a.h_out              = tangent;
        a.h_in               = -tangent;
    }
    return out;
}

std::vector<Vec2d> GLGizmoAdvancedCut::curve_outline(bool with_hover) const
{
    if (m_shape_mode)
        return shape_outline(m_curve_shape);

    std::vector<CurveAnchor> anchors = m_curve_anchors;
    if (with_hover && m_curve_hover && m_curve_drawing && m_curve_drag_idx < 0 && m_curve_bend_span < 0 && !anchors.empty()) {
        CurveAnchor hover;
        hover.p = *m_curve_hover;
        anchors.push_back(hover);
    }
    anchors = curve_effective_anchors(anchors);
    std::vector<Vec2d> out;
    if (anchors.empty())
        return out;
    for (size_t i = 0; i + 1 < anchors.size(); ++i) {
        const CurveAnchor &a = anchors[i], &b = anchors[i + 1];
        const int          n = (a.h_out.isZero() && b.h_in.isZero()) ? 1 : 24;
        for (int s = 0; s < n; ++s)
            out.push_back(bezier(a.p, a.p + a.h_out, b.p + b.h_in, b.p, double(s) / n));
    }
    out.push_back(anchors.back().p);
    return out;
}

TriangleMesh GLGizmoAdvancedCut::curve_cutter_mesh() const
{
    const std::vector<Vec2d> pts = curve_outline(false);
    if (pts.size() < (m_shape_mode ? 3u : 2u))
        return {};
    const double       big = std::max(10.0, 4.0 * (m_radius + (m_bb_center - m_plane_center).norm()));
    std::vector<Vec2d> outline;
    if (m_shape_mode) {
        outline = pts;
    } else {
        // extend both ends past the object, then close on the left side of the path
        const Vec2d d0    = (pts.front() - pts[1]).normalized();
        const Vec2d d1    = (pts.back() - pts[pts.size() - 2]).normalized();
        const Vec2d start = pts.front() + d0 * big, end = pts.back() + d1 * big;
        Vec2d       chord = end - start;
        if (chord.norm() < EPSILON)
            chord = Vec2d(1.0, 0.0);
        const Vec2d left = Vec2d(-chord.y(), chord.x()).normalized();
        outline.push_back(start);
        outline.insert(outline.end(), pts.begin(), pts.end());
        outline.push_back(end);
        outline.push_back(end + left * 2.0 * big);
        outline.push_back(start + left * 2.0 * big);
    }
    Polygon poly;
    for (const Vec2d &p : outline)
        poly.points.emplace_back(scale_(p.x()), scale_(p.y()));
    // union_ex fixes orientation and self intersections of a hand drawn path
    const ExPolygons shapes = union_ex(Polygons{poly});
    if (shapes.empty())
        return {};
    indexed_triangle_set its = extrude_shapes(shapes, big);
    its_transform(its, curve_plane_matrix());
    return TriangleMesh(its);
}

// ---------------------------------------------------------------------------------------------------------------------
// State, undo, preview

void GLGizmoAdvancedCut::curve_reset()
{
    m_curve_anchors.clear();
    m_curve_shape.center   = Vec2d::Zero();
    m_curve_shape.size     = Vec2d::Zero();
    m_curve_shape.rotation = 0.0;
    m_curve_hover.reset();
    m_curve_drag_idx  = -1;
    m_curve_bend_span = -1;
    m_shape_drag      = 0;
    m_curve_drawing   = true;
    m_curve_undo.clear();
    m_curve_redo.clear();
    curve_changed();
}

void GLGizmoAdvancedCut::curve_push_undo()
{
    m_curve_undo.push_back({m_curve_anchors, m_curve_drawing, m_curve_shape});
    if (m_curve_undo.size() > 200)
        m_curve_undo.erase(m_curve_undo.begin());
    m_curve_redo.clear();
}

bool GLGizmoAdvancedCut::curve_undo(bool redo)
{
    if (!m_curve_mode || m_connectors_editing || m_curve_drag_idx >= 0 || m_curve_bend_span >= 0 || m_shape_drag != 0)
        return false;
    std::vector<CurveState> &from = redo ? m_curve_redo : m_curve_undo;
    std::vector<CurveState> &to   = redo ? m_curve_undo : m_curve_redo;
    if (from.empty())
        return false;
    to.push_back({m_curve_anchors, m_curve_drawing, m_curve_shape});
    m_curve_anchors = std::move(from.back().anchors);
    m_curve_drawing = from.back().drawing;
    m_curve_shape   = from.back().shape;
    from.pop_back();
    curve_changed();
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoAdvancedCut::curve_preview_ready() const
{
    return m_curve_mode && !m_curve_pieces.empty() && m_curve_preview_shown == m_curve_generation;
}

bool GLGizmoAdvancedCut::curve_preview_visible() const
{
    return m_curve_mode && !m_curve_pieces.empty();
}

void GLGizmoAdvancedCut::curve_changed()
{
    ++m_curve_generation;
    m_curve_last_plane = curve_plane_matrix();
    // The last pieces stay visible until the new preview is ready, so adding a point does not make the
    // model flash back in between.
    if (!m_curve_preview)
        m_curve_preview = std::make_shared<CurvePreview>();
    const size_t gen = m_curve_generation;
    {
        std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
        m_curve_preview->wanted = gen;
    }
    const bool has_geometry = m_shape_mode ? m_curve_shape.valid() : m_curve_anchors.size() >= 2;
    if (!m_curve_mode || !has_geometry || m_state != On) {
        m_curve_pieces.clear();
        return;
    }

    const Selection &selection    = m_parent.get_selection();
    const int        object_idx   = selection.get_object_idx();
    const int        instance_idx = selection.get_instance_idx();
    const Model     *model        = selection.get_model();
    if (model == nullptr || object_idx < 0 || object_idx >= int(model->objects.size()) || instance_idx < 0)
        return;
    const ModelObject *mo = model->objects[object_idx];
    if (instance_idx >= int(mo->instances.size()))
        return;
    TriangleMesh cutter = curve_cutter_mesh();
    if (cutter.empty())
        return;
    std::vector<std::pair<size_t, TriangleMesh>> parts; // volume index, mesh in world coordinates
    const Transform3d inst_trafo = mo->instances[instance_idx]->get_matrix();
    for (size_t v = 0; v < mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part()) {
            parts.emplace_back(v, mo->volumes[v]->mesh());
            parts.back().second.transform(inst_trafo * mo->volumes[v]->get_matrix(), true);
        }

    std::thread([state = m_curve_preview, gen, cutter = std::move(cutter), parts = std::move(parts)]() {
        // A newer outline makes this run obsolete: stop the boolean as soon as possible.
        const std::function<bool()> cancelled = [&state, gen]() {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->wanted != gen;
        };
        bool                             ok = true;
        std::vector<CurvePreview::Piece> pieces;
        for (const auto &[volume, mesh] : parts) {
            if (cancelled())
                return;
            for (bool inside : {true, false}) {
                const indexed_triangle_set result = mesh_boolean(mesh, cutter, inside, ok, cancelled);
                if (result.indices.empty())
                    continue;
                for (indexed_triangle_set &piece : its_split(result))
                    pieces.push_back({volume, inside, std::move(piece)});
            }
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->wanted != gen)
                return;
            state->pieces = std::move(pieces);
            state->failed = !ok;
            state->ready  = gen;
        }
        if (wxTheApp)
            wxTheApp->CallAfter([]() {
                if (Plater *plater = wxGetApp().plater())
                    if (GLCanvas3D *canvas = plater->get_current_canvas3D()) {
                        canvas->set_as_dirty();
                        canvas->request_extra_frame();
                    }
            });
    }).detach();
}

void GLGizmoAdvancedCut::curve_update_pieces()
{
    if (!m_curve_preview || m_curve_preview_shown == m_curve_generation)
        return;
    std::vector<CurvePreview::Piece> pieces;
    {
        std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
        if (m_curve_preview->ready != m_curve_generation)
            return;
        if (m_curve_preview->failed) {
            m_curve_preview_shown = m_curve_generation;
            m_curve_pieces.clear(); // nothing to show or cut
            return;
        }
        pieces = m_curve_preview->pieces; // copy: the shared state stays valid for later reads
    }
    m_curve_pieces.clear();
    for (CurvePreview::Piece &p : pieces) {
        auto piece    = std::make_shared<CurvePiece>();
        piece->volume = p.volume;
        piece->in_a   = p.in_a;
        piece->model  = std::make_shared<GLModel>();
        piece->model->init_from(p.its);
        piece->mesh      = std::make_shared<TriangleMesh>(std::move(p.its));
        piece->raycaster = std::make_shared<MeshRaycaster>(*piece->mesh);
        m_curve_pieces.push_back(std::move(piece));
    }
    m_curve_preview_shown = m_curve_generation;
}

int GLGizmoAdvancedCut::curve_pick_piece(const Vec2d &mouse_pos) const
{
    const Camera &camera     = wxGetApp().plater()->get_camera();
    const Vec3d   camera_pos = camera.get_position();
    int           best       = -1;
    double        best_dist  = std::numeric_limits<double>::max();
    for (size_t i = 0; i < m_curve_pieces.size(); ++i) {
        Vec3f pos, normal;
        if (m_curve_pieces[i]->raycaster->unproject_on_mesh(mouse_pos, Transform3d::Identity(), camera, pos, normal)) {
            const double d = (camera_pos - pos.cast<double>()).squaredNorm();
            if (d < best_dist) {
                best_dist = d;
                best      = int(i);
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mouse

bool GLGizmoAdvancedCut::curve_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down)
{
    if (!m_curve_mode || m_connectors_editing || m_state != On)
        return false;
    if (m_shape_mode)
        return shape_gizmo_event(action, mouse_position, shift_down);

    const double tolerance = std::max(0.5, m_radius * 0.04); // pick distance in the plane, mm
    const size_t n         = m_curve_anchors.size();
    auto anchor_at = [this, n, tolerance](const Vec2d &local) {
        for (size_t i = 0; i < n; ++i)
            if ((local - m_curve_anchors[i].p).norm() < tolerance)
                return int(i);
        return -1;
    };
    // restore the state saved when the current drag started
    auto cancel_drag = [this]() {
        if (!m_curve_undo.empty()) {
            m_curve_anchors = std::move(m_curve_undo.back().anchors);
            m_curve_drawing = m_curve_undo.back().drawing;
            m_curve_undo.pop_back();
        }
        m_curve_drag_idx  = -1;
        m_curve_bend_span = -1;
    };

    switch (action) {
    case SLAGizmoEventType::LeftDown: {
        // The move/rotate grabbers keep working; the plane itself is a drawing surface here.
        if (m_hover_id != -1 && m_hover_id != CURVE_PLATE_MOVE_ID)
            return false;
        Vec2d local;
        if (!curve_unproject(mouse_position, local))
            return false;
        // grab a point to move it
        if (const int idx = anchor_at(local); idx >= 0) {
            curve_push_undo(); // state before the move, also used by right-click to cancel it
            m_curve_drag_idx    = idx;
            m_curve_drag_handle = false;
            m_parent.set_as_dirty();
            return true;
        }
        if (m_curve_drawing) {
            // add a point: smooth by default, Shift = corner; dragging now pulls out handles
            curve_push_undo();
            CurveAnchor a;
            a.p      = local;
            a.corner = shift_down;
            m_curve_anchors.push_back(a);
            m_curve_drag_idx    = int(m_curve_anchors.size()) - 1;
            m_curve_drag_handle = true;
            m_parent.set_as_dirty();
            return true;
        }
        // grab the line to bend it
        const std::vector<CurveAnchor> eff = curve_effective_anchors(m_curve_anchors);
        double best = tolerance;
        for (size_t i = 0; i + 1 < n; ++i) {
            const CurveAnchor &a = eff[i], &b = eff[i + 1];
            for (int k = 1; k < 32; ++k) {
                const double t = double(k) / 32.0;
                const double d = (bezier(a.p, a.p + a.h_out, b.p + b.h_in, b.p, t) - local).norm();
                if (d < best) {
                    best              = d;
                    m_curve_bend_span = int(i);
                    m_curve_bend_t    = std::clamp(t, 0.15, 0.85);
                }
            }
        }
        if (m_curve_bend_span >= 0) {
            curve_push_undo();
            m_parent.set_as_dirty();
            return true;
        }
        return false; // not on the path: let the view rotate
    }
    case SLAGizmoEventType::Dragging: {
        Vec2d local;
        if (m_curve_bend_span >= 0) {
            if (!m_curve_undo.empty() && m_curve_undo.back().anchors.size() == n && m_curve_bend_span + 1 < int(n) &&
                curve_unproject(mouse_position, local)) {
                // Move the grabbed spot of the curve to the mouse: shifting both inner control points
                // by D / (3 u t) moves B(t) by exactly D.
                const std::vector<CurveAnchor> start = curve_effective_anchors(m_curve_undo.back().anchors);
                const size_t ia = size_t(m_curve_bend_span), ib = ia + 1;
                const CurveAnchor &a = start[ia], &b = start[ib];
                const double t = m_curve_bend_t, u = 1.0 - t;
                const Vec2d  d = (local - bezier(a.p, a.p + a.h_out, b.p + b.h_in, b.p, t)) / (3.0 * u * t);
                m_curve_anchors[ia] = a;
                m_curve_anchors[ib] = b;
                m_curve_anchors[ia].manual = m_curve_anchors[ib].manual = true;
                m_curve_anchors[ia].corner = m_curve_anchors[ib].corner = false;
                m_curve_anchors[ia].h_out += d;
                m_curve_anchors[ib].h_in += d;
                m_parent.set_as_dirty();
            }
            return true;
        }
        if (m_curve_drag_idx < 0 || m_curve_drag_idx >= int(n))
            return false;
        if (curve_unproject(mouse_position, local)) {
            CurveAnchor &a = m_curve_anchors[m_curve_drag_idx];
            if (m_curve_drag_handle) {
                const Vec2d d = local - a.p;
                if (d.norm() > tolerance * 0.5) { // pulling handles out makes it a manual smooth point
                    a.manual = true;
                    a.corner = false;
                    a.h_out  = d;
                    a.h_in   = -d;
                }
            } else {
                a.p = local;
            }
            m_parent.set_as_dirty();
        }
        return true;
    }
    case SLAGizmoEventType::LeftUp:
        if (m_curve_bend_span >= 0) {
            m_curve_bend_span = -1;
            curve_changed();
            m_parent.set_as_dirty();
            return true;
        }
        if (m_curve_drag_idx < 0)
            return false;
        if (!m_curve_drag_handle && !m_curve_undo.empty() && m_curve_drag_idx < int(m_curve_undo.back().anchors.size()) &&
            m_curve_undo.back().anchors[m_curve_drag_idx].p.isApprox(m_curve_anchors[m_curve_drag_idx].p)) {
            // a click on a point without moving it: nothing to undo...
            m_curve_undo.pop_back();
            // ...but a click on the last point of a stopped path continues drawing
            if (!m_curve_drawing && m_curve_drag_idx == int(n) - 1) {
                curve_push_undo();
                m_curve_drawing = true;
            }
            m_curve_drag_idx = -1;
            m_parent.set_as_dirty();
            return true;
        }
        m_curve_drag_idx = -1;
        curve_changed();
        m_parent.set_as_dirty();
        return true;
    case SLAGizmoEventType::RightDown: {
        // Right-click: cancel a drag; remove the point under the mouse; stop drawing; move a preview
        // piece between A and B - the first that applies.
        if (m_curve_drag_idx >= 0 || m_curve_bend_span >= 0) {
            cancel_drag();
            curve_changed();
            m_parent.set_as_dirty();
            return true;
        }
        Vec2d local;
        if (curve_unproject(mouse_position, local))
            if (const int idx = anchor_at(local); idx >= 0) {
                curve_push_undo();
                m_curve_anchors.erase(m_curve_anchors.begin() + idx);
                curve_changed();
                m_parent.set_as_dirty();
                return true;
            }
        if (m_curve_drawing && n > 0) {
            curve_push_undo();
            m_curve_drawing = false;
            m_parent.set_as_dirty();
            return true;
        }
        if (const int piece = curve_preview_ready() ? curve_pick_piece(mouse_position) : -1; piece >= 0) {
            m_curve_pieces[piece]->in_a = !m_curve_pieces[piece]->in_a;
            m_parent.set_as_dirty();
        }
        return true;
    }
    case SLAGizmoEventType::RightUp: return true;
    default: return false;
    }
}

bool GLGizmoAdvancedCut::shape_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down)
{
    auto to_shape_frame = [this](const Vec2d &local) { return rotate2d(local - m_curve_shape.center, -m_curve_shape.rotation); };
    switch (action) {
    case SLAGizmoEventType::LeftDown: {
        if (m_hover_id != -1 && m_hover_id != CURVE_PLATE_MOVE_ID)
            return false;
        Vec2d local;
        if (!curve_unproject(mouse_position, local))
            return false;
        curve_push_undo();
        const Vec2d q = to_shape_frame(local);
        if (m_curve_shape.valid() && std::abs(q.x()) <= 0.5 * m_curve_shape.size.x() && std::abs(q.y()) <= 0.5 * m_curve_shape.size.y()) {
            m_shape_drag       = 2; // inside the shape: move it
            m_shape_drag_start = local - m_curve_shape.center;
        } else {
            m_shape_drag         = 1; // outside: draw a new one from its centre outwards
            m_curve_shape.center = local;
            m_curve_shape.size   = Vec2d::Zero();
        }
        m_parent.set_as_dirty();
        return true;
    }
    case SLAGizmoEventType::Dragging: {
        Vec2d local;
        if (m_shape_drag == 0 || !curve_unproject(mouse_position, local))
            return m_shape_drag != 0;
        if (m_shape_drag == 2) {
            m_curve_shape.center = local - m_shape_drag_start;
        } else {
            const Vec2d q = to_shape_frame(local);
            Vec2d size(2.0 * std::abs(q.x()), 2.0 * std::abs(q.y()));
            if (shift_down || m_curve_shape.type == ShapeType::Circle)
                size = Vec2d::Constant(size.maxCoeff()); // keep it round / square
            m_curve_shape.size = size;
        }
        m_parent.set_as_dirty();
        return true;
    }
    case SLAGizmoEventType::LeftUp:
        if (m_shape_drag == 0)
            return false;
        if (m_shape_drag == 1 && !m_curve_shape.valid() && !m_curve_undo.empty()) {
            // a click without dragging: keep the previous shape
            m_curve_shape = m_curve_undo.back().shape;
            m_curve_undo.pop_back();
        }
        m_shape_drag = 0;
        curve_changed();
        m_parent.set_as_dirty();
        return true;
    case SLAGizmoEventType::RightDown:
        if (m_shape_drag != 0) { // cancel the drag
            if (!m_curve_undo.empty()) {
                m_curve_shape = m_curve_undo.back().shape;
                m_curve_undo.pop_back();
            }
            m_shape_drag = 0;
            curve_changed();
        } else if (const int piece = curve_preview_ready() ? curve_pick_piece(mouse_position) : -1; piece >= 0) {
            m_curve_pieces[piece]->in_a = !m_curve_pieces[piece]->in_a;
        }
        m_parent.set_as_dirty();
        return true;
    case SLAGizmoEventType::RightUp: return true;
    default: return false;
    }
}

void GLGizmoAdvancedCut::curve_on_mouse(const wxMouseEvent &mouse_event)
{
    if (!m_curve_mode || m_shape_mode || m_connectors_editing || m_state != On)
        return;
    if (mouse_event.Moving()) {
        Vec2d local;
        if (curve_unproject(Vec2d(mouse_event.GetX(), mouse_event.GetY()), local))
            m_curve_hover = local;
        else
            m_curve_hover.reset();
        m_parent.request_extra_frame();
    } else if (mouse_event.Leaving()) {
        m_curve_hover.reset();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering and panel

void GLGizmoAdvancedCut::render_curve()
{
    if (!m_curve_mode || m_connectors_editing)
        return;

    // The plane moved or rotated: the cutter changed, recompute the preview once dragging stops.
    if (!m_dragging && !curve_plane_matrix().isApprox(m_curve_last_plane))
        curve_changed();
    curve_update_pieces();

    const Camera &camera = wxGetApp().plater()->get_camera();

    // Preview pieces in the A / B colours
    glsafe(::glEnable(GL_DEPTH_TEST));
    for (const auto &piece : m_curve_pieces) {
        if (piece->in_a ? !m_keep_upper : !m_keep_lower)
            continue;
        render_glmodel(*piece->model, (piece->in_a ? CURVE_PART_A_COLOR : CURVE_PART_B_COLOR).get_data(), camera.get_view_matrix(),
                       camera.get_projection_matrix());
    }

    // Outline, points and handles
    const std::vector<Vec2d> outline = curve_outline(true);
    if (outline.empty())
        return;
    const auto &shader = wxGetApp().get_shader("flat");
    if (!shader)
        return;

    const Transform3d plane = curve_plane_matrix();
    auto world = [&plane](const Vec2d &p) -> Vec3f { return (plane * Vec3d(p.x(), p.y(), 0.0)).cast<float>(); };

    GLModel::Geometry geo;
    geo.format.type          = GLModel::PrimitiveType::Lines;
    geo.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3;
    unsigned int idx         = 0;
    auto add_segment = [&](const Vec2d &a, const Vec2d &b) {
        geo.add_vertex(world(a));
        geo.add_vertex(world(b));
        geo.add_line(idx, idx + 1);
        idx += 2;
    };
    for (size_t i = 0; i + 1 < outline.size(); ++i)
        add_segment(outline[i], outline[i + 1]);
    if (m_shape_mode && outline.size() > 2)
        add_segment(outline.back(), outline.front());
    const double s = std::max(0.4, m_radius * 0.02);
    auto add_square = [&](const Vec2d &p, double r) {
        add_segment(p + Vec2d(-r, -r), p + Vec2d(r, -r));
        add_segment(p + Vec2d(r, -r), p + Vec2d(r, r));
        add_segment(p + Vec2d(r, r), p + Vec2d(-r, r));
        add_segment(p + Vec2d(-r, r), p + Vec2d(-r, -r));
    };
    if (m_shape_mode) {
        if (m_curve_shape.valid()) {
            add_segment(m_curve_shape.center - Vec2d(s, 0), m_curve_shape.center + Vec2d(s, 0));
            add_segment(m_curve_shape.center - Vec2d(0, s), m_curve_shape.center + Vec2d(0, s));
        }
    } else {
        for (const CurveAnchor &a : curve_effective_anchors(m_curve_anchors)) {
            add_square(a.p, a.corner ? s * 0.7 : s);
            if (a.manual) // only user-set handles are shown, the automatic ones would be noise
                for (const Vec2d &h : {a.h_in, a.h_out})
                    if (!h.isZero()) {
                        add_segment(a.p, a.p + h);
                        add_segment(a.p + h - Vec2d(s, 0), a.p + h + Vec2d(s, 0));
                        add_segment(a.p + h - Vec2d(0, s), a.p + h + Vec2d(0, s));
                    }
        }
    }
    if (idx == 0)
        return;

    m_curve_model.reset();
    m_curve_model.init_from(std::move(geo));
    m_curve_model.set_color({1.0f, 0.55f, 0.0f, 1.0f});

    glsafe(::glDisable(GL_DEPTH_TEST)); // always visible, also through the object
    glsafe(::glLineWidth(2.0f));
    wxGetApp().bind_shader(shader);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    m_curve_model.render_geometry();
    wxGetApp().unbind_shader();
    glsafe(::glLineWidth(1.0f));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLGizmoAdvancedCut::render_curve_panel()
{
    if (m_shape_mode) {
        m_imgui->text(_L("Drag on the cut plane to draw the shape from its centre (Shift: keep it round or square)."));
        m_imgui->text(_L("Drag inside the shape to move it."));
        const std::vector<std::string> types = {_u8L("Circle"), _u8L("Ellipse"), _u8L("Rectangle"), _u8L("Rounded rectangle"), _u8L("Polygon")};
        size_t type_idx = size_t(m_curve_shape.type);
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (render_combo(_u8L("Shape"), types, type_idx, m_label_width, m_imgui->scaled(10.f))) {
            curve_push_undo();
            m_curve_shape.type = ShapeType(type_idx);
            if (m_curve_shape.type == ShapeType::Circle)
                m_curve_shape.size = Vec2d::Constant(m_curve_shape.size.maxCoeff());
            curve_changed();
        }
        ImGuiWrapper::pop_combo_style();

        // numeric editing; one undo step per edited field
        bool changed = false;
        auto edit = [&](const wxString &label, double &value, double min_value) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(label);
            ImGui::SameLine(m_label_width);
            ImGui::PushItemWidth(m_imgui->scaled(6.f));
            double v = value;
            if (ImGui::InputDouble(("##shape_" + label.ToStdString()).c_str(), &v, 0.0, 0.0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue) &&
                std::abs(v - value) > EPSILON) {
                curve_push_undo();
                value   = std::max(min_value, v);
                changed = true;
            }
            ImGui::PopItemWidth();
        };
        double width = m_curve_shape.size.x(), height = m_curve_shape.size.y();
        edit(_L("Width"), width, 0.0);
        if (m_curve_shape.type != ShapeType::Circle)
            edit(_L("Height"), height, 0.0);
        else
            height = width;
        if (m_curve_shape.type == ShapeType::RoundedRect)
            edit(_L("Corner radius"), m_curve_shape.corner_radius, 0.0);
        if (m_curve_shape.type == ShapeType::Polygon) {
            double sides = m_curve_shape.sides;
            edit(_L("Sides"), sides, 3.0);
            m_curve_shape.sides = std::clamp(int(std::lround(sides)), 3, 64);
        }
        edit(_L("Rotation"), m_curve_shape.rotation, -360.0);
        m_curve_shape.size = Vec2d(width, height);
        if (changed)
            curve_changed();
    } else {
        if (m_curve_drawing) {
            m_imgui->text(_L("Drawing:"));
            m_imgui->text(" - " + _L("Click to add points; the line runs smoothly through them"));
            m_imgui->text(" - " + _L("Shift+click: sharp corner.  Click and drag: set the curve by hand"));
            m_imgui->text(" - " + _L("Right-click: stop drawing"));
        } else {
            m_imgui->text(_L("Editing:"));
            m_imgui->text(" - " + _L("Drag the line to bend it, drag a point to move it"));
            m_imgui->text(" - " + _L("Click the last point to continue drawing"));
        }
        m_imgui->text(" - " + _L("Right-click a point to remove it"));
    }
    m_imgui->text(" - " + _L("Right-click a coloured piece to move it to the other side"));
    m_imgui->text(" - " + _L("Ctrl+Z: undo.  Ctrl+Y: redo.  Ctrl+drag: rotate the view"));
    ImGui::Separator();

    const bool empty = m_shape_mode ? !m_curve_shape.valid() : m_curve_anchors.empty();
    m_imgui->disabled_begin(empty);
    if (m_imgui->button(_L("Clear"))) {
        curve_push_undo(); // undoable with Ctrl+Z
        m_curve_anchors.clear();
        m_curve_shape.size = Vec2d::Zero();
        m_curve_drawing    = true;
        curve_changed();
    }
    m_imgui->disabled_end();
    if (!m_shape_mode && !m_curve_drawing && !m_curve_anchors.empty()) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Continue drawing"))) {
            curve_push_undo();
            m_curve_drawing = true;
        }
    }

    const bool has_geometry = m_shape_mode ? m_curve_shape.valid() : m_curve_anchors.size() >= 2;
    if (has_geometry && m_curve_drag_idx < 0 && m_curve_bend_span < 0 && m_shape_drag == 0) {
        bool failed = false, ready = false;
        if (m_curve_preview) {
            std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
            ready  = m_curve_preview->ready == m_curve_generation;
            failed = ready && m_curve_preview->failed;
        }
        m_imgui->text(failed ? _L("Preview failed: the model may need repairing.") :
                      ready  ? (m_curve_pieces.empty() ? _L("The outline does not cross the model.") : _L("Preview: part A cyan, part B magenta.")) :
                      m_curve_pieces.empty() ? _L("Calculating preview...") : _L("Updating preview..."));
    }
    ImGui::Separator();
    m_imgui->text(_L("After cut") + ": ");
    m_imgui->bbl_checkbox(_L("Keep part A"), m_keep_upper);
    ImGui::SameLine();
    m_imgui->bbl_checkbox(_L("Keep part B"), m_keep_lower);
}

bool GLGizmoAdvancedCut::can_perform_curve_cut() const
{
    if (!(m_keep_upper || m_keep_lower) || m_curve_drag_idx >= 0 || m_curve_bend_span >= 0 || m_shape_drag != 0)
        return false;
    // the cut uses exactly the previewed pieces, so both sides must exist
    if (!curve_preview_ready())
        return false;
    bool any_a = false, any_b = false;
    for (const auto &piece : m_curve_pieces)
        (piece->in_a ? any_a : any_b) = true;
    return any_a && any_b;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cut

void GLGizmoAdvancedCut::perform_curve_cut(const Selection &selection)
{
    if (!can_perform_curve_cut())
        return;
    const int instance_idx = selection.get_instance_idx();
    const int object_idx   = selection.get_object_idx();
    Plater   *plater       = wxGetApp().plater();
    if (instance_idx < 0 || object_idx < 0 || object_idx >= int(plater->model().objects.size()))
        return;
    const ModelObject *mo = plater->model().objects[object_idx];
    if (mo == nullptr || instance_idx >= int(mo->instances.size()))
        return;
    // The cut makes new triangles, so painted colour, supports, seams and fuzzy skin are lost.
    if ((mo->is_mm_painted() || mo->is_fuzzy_skin_painted() || mo->is_fdm_support_painted() || mo->is_seam_painted()) &&
        !wxGetApp().confirm_mesh_paint_warning())
        return;

    // Results per side and volume, from the previewed pieces (world -> volume coordinates)
    const Transform3d inst_trafo = mo->instances[instance_idx]->get_matrix();
    auto collect = [&](bool side_a) {
        std::vector<std::optional<indexed_triangle_set>> meshes(mo->volumes.size());
        for (size_t v = 0; v < mo->volumes.size(); ++v)
            if (mo->volumes[v]->is_model_part())
                meshes[v] = indexed_triangle_set();
        for (const auto &piece : m_curve_pieces)
            if (piece->in_a == side_a && piece->volume < meshes.size() && meshes[piece->volume])
                its_merge(*meshes[piece->volume], piece->mesh->its);
        for (size_t v = 0; v < meshes.size(); ++v)
            if (meshes[v] && !meshes[v]->indices.empty())
                its_transform(*meshes[v], (inst_trafo * mo->volumes[v]->get_matrix()).inverse(), true);
        return meshes;
    };

    // Build the result objects in a scratch model
    Model           scratch;
    ModelObjectPtrs results;
    auto build = [&](std::vector<std::optional<indexed_triangle_set>> meshes, const std::string &suffix) {
        ModelObject *obj = scratch.add_object(*mo);
        obj->name += suffix;
        obj->clear_instances();
        obj->add_instance(*mo->instances[instance_idx]);
        obj->cut_connectors.clear();
        obj->cut_id.invalidate();
        for (int v = int(obj->volumes.size()) - 1; v >= 0; --v) {
            ModelVolume *vol = obj->volumes[v];
            if (!meshes[v])
                continue; // modifiers etc. are kept as they are
            if (meshes[v]->indices.empty()) {
                obj->delete_volume(size_t(v));
                continue;
            }
            vol->set_mesh(std::move(*meshes[v]));
            vol->calculate_convex_hull();
            // painted data refers to the old triangles
            vol->supported_facets.reset();
            vol->seam_facets.reset();
            vol->mmu_segmentation_facets.reset();
            vol->fuzzy_skin_facets.reset();
            vol->set_new_unique_id();
        }
        obj->invalidate_bounding_box();
        if (std::any_of(obj->volumes.begin(), obj->volumes.end(), [](const ModelVolume *v) { return v->is_model_part(); }))
            results.push_back(obj);
    };
    if (m_keep_upper)
        build(collect(true), " (A)");
    if (m_keep_lower)
        build(collect(false), " (B)");
    if (results.empty())
        return;

    m_parent.reset_all_gizmos();
    Plater::TakeSnapshot snapshot(plater, m_shape_mode ? "Cut by shape" : "Cut by curve");
    plater->apply_cut_object_to_model(size_t(object_idx), results);
    curve_reset();
}

} // namespace GUI
} // namespace Slic3r
