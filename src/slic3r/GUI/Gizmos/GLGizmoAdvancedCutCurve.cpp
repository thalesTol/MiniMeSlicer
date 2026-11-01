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
#include <tuple>

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

// Closed solid from 2D shapes (plane frame, mm): caps at z = z0 / z1 plus side walls.
indexed_triangle_set extrude_shapes(const ExPolygons &shapes, double z0, double z1)
{
    std::vector<Vec3d> soup;
    for (const ExPolygon &shape : shapes) {
        // already unscaled to mm by the tesselator
        std::vector<Vec3d> top    = triangulate_expolygon_3d(shape, z1, false);
        std::vector<Vec3d> bottom = triangulate_expolygon_3d(shape, z0, true);
        soup.insert(soup.end(), top.begin(), top.end());
        soup.insert(soup.end(), bottom.begin(), bottom.end());
        // Contour is CCW and holes CW, so (a, b) with "up" gives outward facing walls for both.
        auto add_walls = [&](const Polygon &poly) {
            const size_t m = poly.points.size();
            for (size_t i = 0; i < m; ++i) {
                const Vec2d a = unscale(poly.points[i]);
                const Vec2d b = unscale(poly.points[(i + 1) % m]);
                const Vec3d a0(a.x(), a.y(), z0), b0(b.x(), b.y(), z0), a1(a.x(), a.y(), z1), b1(b.x(), b.y(), z1);
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

// A op B ("INTERSECTION", "A_NOT_B" or "UNION") with the default backend, CGAL as fallback.
// 'ok' is set to false when both failed. 'cancel' (optional) is polled by the boolean backend; a
// cancelled run returns an empty mesh.
indexed_triangle_set mesh_boolean_op(const TriangleMesh &a, const TriangleMesh &b, const std::string &op, bool &ok,
                                     const std::function<bool()> &cancel = nullptr)
{
    std::vector<TriangleMesh> results;
    bool failed = false;
    MeshBoolean::mcut::make_boolean(a, b, results, op, cancel, nullptr, [&failed]() { failed = true; });
    if (cancel && cancel())
        return {};
    indexed_triangle_set its;
    if (!results.empty()) {
        for (const TriangleMesh &r : results)
            its_merge(its, r.its);
    } else if (failed) {
        try {
            TriangleMesh tmp = a;
            if (op == "INTERSECTION")
                MeshBoolean::cgal::intersect(tmp, b);
            else if (op == "UNION")
                MeshBoolean::cgal::plus(tmp, b);
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

indexed_triangle_set mesh_boolean(const TriangleMesh &a, const TriangleMesh &b, bool intersect, bool &ok,
                                  const std::function<bool()> &cancel = nullptr)
{
    return mesh_boolean_op(a, b, intersect ? "INTERSECTION" : "A_NOT_B", ok, cancel);
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

// Arc-length access to a polyline (closed or open).
struct OutlineSampler
{
    std::vector<Vec2d>  pts;
    std::vector<double> acc; // length up to point i
    double              total{0.0};

    OutlineSampler(const std::vector<Vec2d> &points, bool closed) : pts(points)
    {
        if (closed && !pts.empty())
            pts.push_back(pts.front());
        acc.assign(pts.size(), 0.0);
        for (size_t i = 1; i < pts.size(); ++i)
            acc[i] = acc[i - 1] + (pts[i] - pts[i - 1]).norm();
        total = acc.empty() ? 0.0 : acc.back();
    }
    bool valid() const { return pts.size() >= 2 && total > EPSILON; }
    // point and unit tangent at fraction t of the length
    void at(double t, Vec2d &p, Vec2d &tangent) const
    {
        const double d = std::clamp(t, 0.0, 1.0) * total;
        size_t       i = 1;
        while (i + 1 < pts.size() && acc[i] < d)
            ++i;
        const double seg = std::max(acc[i] - acc[i - 1], EPSILON);
        p                = pts[i - 1] + (pts[i] - pts[i - 1]) * ((d - acc[i - 1]) / seg);
        tangent          = (pts[i] - pts[i - 1]).normalized();
    }
    // fraction of the length of the point nearest to q, and its distance
    double nearest(const Vec2d &q, double &dist) const
    {
        double best_t = 0.0;
        dist          = std::numeric_limits<double>::max();
        for (size_t i = 1; i < pts.size(); ++i) {
            const Vec2d  ab = pts[i] - pts[i - 1];
            const double l2 = ab.squaredNorm();
            const double u  = l2 > 0.0 ? std::clamp((q - pts[i - 1]).dot(ab) / l2, 0.0, 1.0) : 0.0;
            const double d  = (pts[i - 1] + u * ab - q).norm();
            if (d < dist) {
                dist   = d;
                best_t = (acc[i - 1] + u * std::sqrt(l2)) / total;
            }
        }
        return best_t;
    }
};

// Connector frame on the cut wall: p = point on the outline, tangent along it, dir = into the other part.
struct ConnFrame
{
    Vec2d p, tangent, dir;
};

// Dovetail cross-section in the plane: narrow at the wall, wider inside the other part.
Polygon dovetail_profile(const ConnFrame &f, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double overlap  = 0.5; // reaches into its own part so the union is clean
    const double half_in  = 0.5 * c.width + extra;
    const double half_out = 0.5 * c.width + c.depth * std::tan(c.angle * M_PI / 180.0) + extra;
    const double out      = c.depth + extra;
    const std::array<Vec2d, 4> q = {f.p - f.dir * overlap - f.tangent * half_in, f.p - f.dir * overlap + f.tangent * half_in,
                                    f.p + f.dir * out + f.tangent * half_out, f.p + f.dir * out - f.tangent * half_out};
    Polygon poly;
    for (const Vec2d &v : q)
        poly.points.emplace_back(scale_(v.x()), scale_(v.y()));
    if (!poly.is_counter_clockwise())
        poly.reverse();
    return poly;
}

// Transform taking local (u along the tangent, v into the other part, w along the plane normal) to the plane frame.
Transform3d conn_local_frame(const ConnFrame &f, double z)
{
    Transform3d m = Transform3d::Identity();
    m.linear().col(0) = Vec3d(f.tangent.x(), f.tangent.y(), 0.0);
    m.linear().col(1) = Vec3d(f.dir.x(), f.dir.y(), 0.0);
    m.linear().col(2) = Vec3d::UnitZ();
    m.translation()   = Vec3d(f.p.x(), f.p.y(), z);
    return m;
}

void orient_outwards(indexed_triangle_set &its)
{
    if (its_volume(its) < 0.0)
        its_flip_triangles(its);
}

// Round pin from the wall into the other part.
indexed_triangle_set pin_solid(const ConnFrame &f, double z, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double overlap = 0.5;
    indexed_triangle_set its = its_make_cylinder(0.5 * c.pin_diameter + extra, c.pin_length + overlap + extra, 2.0 * M_PI / 48.0);
    // cylinder axis is +Z: lay it along v, starting 'overlap' inside its own part
    Transform3d lay = Transform3d::Identity();
    lay.linear().col(0) = Vec3d::UnitX();
    lay.linear().col(1) = Vec3d::UnitZ();
    lay.linear().col(2) = Vec3d::UnitY();
    lay.translation()   = Vec3d(0.0, -overlap, 0.0);
    its_transform(its, conn_local_frame(f, z) * lay, true);
    orient_outwards(its);
    return its;
}

// Snap-fit barb: a ramp along the sliding direction ending in a flat catch face.
indexed_triangle_set snap_solid(const ConnFrame &f, double z, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double overlap = 0.5;
    const double hu = 0.5 * c.width + extra, h0 = z - 0.5 * c.snap_height - extra, h1 = z + 0.5 * c.snap_height + extra;
    const double tip = c.depth + extra;
    // catch face at h0 (flat), the ramp rises towards it from h1; 'reverse' swaps the direction
    const double catch_w = c.reverse ? h1 : h0, ramp_w = c.reverse ? h0 : h1;
    indexed_triangle_set its;
    for (double u : {-hu, hu}) {
        its.vertices.emplace_back(Vec3f(float(u), float(-overlap), float(catch_w)));
        its.vertices.emplace_back(Vec3f(float(u), float(tip), float(catch_w)));
        its.vertices.emplace_back(Vec3f(float(u), float(-overlap), float(ramp_w)));
    }
    // two triangular caps, three quads
    its.indices = {{0, 2, 1}, {3, 4, 5}, {0, 1, 4}, {0, 4, 3}, {1, 2, 5}, {1, 5, 4}, {2, 0, 3}, {2, 3, 5}};
    its_transform(its, conn_local_frame(f, 0.0), true);
    orient_outwards(its);
    return its;
}

// Dovetail with a closed end: the prism only covers part of the wall height.
indexed_triangle_set dovetail_solid(const ConnFrame &f, double z0, double z1, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    return extrude_shapes(union_ex(Polygons{dovetail_profile(f, c, extra)}), z0, z1);
}

// Bowtie (butterfly) key across the wall: narrow at the wall, wide on both sides.
Polygon bowtie_profile(const ConnFrame &f, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double hn = 0.5 * c.width + extra;
    const double ho = 0.5 * c.width + c.depth * std::tan(c.angle * M_PI / 180.0) + extra;
    const double d  = c.depth + extra;
    const std::array<Vec2d, 6> q = {f.p - f.dir * d - f.tangent * ho, f.p - f.dir * d + f.tangent * ho, f.p + f.tangent * hn,
                                    f.p + f.dir * d + f.tangent * ho, f.p + f.dir * d - f.tangent * ho, f.p - f.tangent * hn};
    Polygon poly;
    for (const Vec2d &v : q)
        poly.points.emplace_back(scale_(v.x()), scale_(v.y()));
    if (!poly.is_counter_clockwise())
        poly.reverse();
    return poly;
}

// Round dowel centred on the wall, half in each part.
indexed_triangle_set dowel_solid(const ConnFrame &f, double z, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double half = 0.5 * c.pin_length + extra;
    indexed_triangle_set its = its_make_cylinder(0.5 * c.pin_diameter + extra, 2.0 * half, 2.0 * M_PI / 48.0);
    Transform3d lay = Transform3d::Identity();
    lay.linear().col(0) = Vec3d::UnitX();
    lay.linear().col(1) = Vec3d::UnitZ();
    lay.linear().col(2) = Vec3d::UnitY();
    lay.translation()   = Vec3d(0.0, -half, 0.0);
    its_transform(its, conn_local_frame(f, z) * lay, true);
    orient_outwards(its);
    return its;
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

bool GLGizmoAdvancedCut::curve_build_cutters(CurveCutters &out) const
{
    const std::vector<Vec2d> pts = curve_outline(false);
    if (pts.size() < (m_shape_mode ? 3u : 2u))
        return false;
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
    const ExPolygons base = union_ex(Polygons{poly});
    if (base.empty())
        return false;

    // Connectors, each with its own settings. Part A lies left of an open path and inside a shape, so
    // "into B" is the right-hand normal of the outline.
    //   owned by A:  cutter a += solid,  cutter b += hole
    //   owned by B:  cutter a -= hole,   cutter b -= solid
    //   separate:    cutter a -= hole,   cutter b += hole, plus the key as its own piece
    Polygons add_a, sub_a, add_b, sub_b; // full-height shapes, done in 2D with the outline
    out.ops.clear();
    out.keys.clear();
    std::vector<std::pair<indexed_triangle_set, bool>> keys_plane; // key, already in plane frame
    const BoundingBoxf3  tbb  = transformed_bounding_box(m_plane_center, m_rotate_matrix); // plane frame
    const double         z_lo = tbb.defined ? tbb.min.z() : -m_radius, z_hi = tbb.defined ? tbb.max.z() : m_radius;
    const OutlineSampler sampler(pts, m_shape_mode);
    std::vector<std::tuple<indexed_triangle_set, bool, bool>> ops_plane; // mesh, on_a, unite
    if (sampler.valid())
        for (const CurveConnector &conn : m_curve_conns) {
            const CurveConnParams &c = conn.params;
            ConnFrame f;
            sampler.at(conn.t, f.p, f.tangent);
            f.dir = Vec2d(f.tangent.y(), -f.tangent.x());
            const bool owner_b = c.on_part_b && !c.separate();
            if (owner_b)
                f.dir = -f.dir; // grows out of part B into part A
            const double tol = c.tolerance;
            const double z   = std::clamp(conn.z, z_lo, z_hi);
            auto add_3d = [&](indexed_triangle_set solid, indexed_triangle_set hole) {
                if (c.separate()) {
                    ops_plane.emplace_back(hole, true, false);
                    ops_plane.emplace_back(std::move(hole), false, true);
                    keys_plane.emplace_back(std::move(solid), true);
                } else if (!owner_b) {
                    ops_plane.emplace_back(std::move(solid), true, true);
                    ops_plane.emplace_back(std::move(hole), false, true);
                } else {
                    ops_plane.emplace_back(std::move(hole), true, false);
                    ops_plane.emplace_back(std::move(solid), false, false);
                }
            };
            switch (c.type) {
            case ConnType::Dovetail:
                if (!c.closed_end) {
                    (owner_b ? sub_b : add_a).push_back(dovetail_profile(f, c, 0.0));
                    (owner_b ? sub_a : add_b).push_back(dovetail_profile(f, c, tol));
                } else {
                    // closed at one end: the slide stops there and the part stays put
                    const double run = (z_hi - z_lo) * std::clamp(c.length, 5.0, 100.0) / 100.0;
                    const double z0 = c.reverse ? -big : z_hi - run, z1 = c.reverse ? z_lo + run : big;
                    add_3d(dovetail_solid(f, z0, z1, c, 0.0), dovetail_solid(f, c.reverse ? z0 : z0 - tol, c.reverse ? z1 + tol : z1, c, tol));
                }
                break;
            case ConnType::Pin: add_3d(pin_solid(f, z, c, 0.0), pin_solid(f, z, c, tol)); break;
            case ConnType::Snap: add_3d(snap_solid(f, z, c, 0.0), snap_solid(f, z, c, tol)); break;
            case ConnType::Dowel: add_3d(dowel_solid(f, z, c, 0.0), dowel_solid(f, z, c, tol)); break;
            case ConnType::Bowtie: {
                // slides in along the wall; the key is as tall as the object
                sub_a.push_back(bowtie_profile(f, c, tol));
                add_b.push_back(bowtie_profile(f, c, tol));
                keys_plane.emplace_back(extrude_shapes(union_ex(Polygons{bowtie_profile(f, c, 0.0)}), z_lo, z_hi), true);
                break;
            }
            }
        }

    // A = object inside cutter a, B = object outside cutter b
    ExPolygons cut_a = base, cut_b = base;
    if (!add_a.empty())
        cut_a = union_ex(cut_a, add_a);
    if (!sub_a.empty())
        cut_a = diff_ex(cut_a, sub_a);
    if (!add_b.empty())
        cut_b = union_ex(cut_b, add_b);
    if (!sub_b.empty())
        cut_b = diff_ex(cut_b, sub_b);
    const Transform3d plane = curve_plane_matrix();
    indexed_triangle_set its_a = extrude_shapes(cut_a, -big, big), its_b = extrude_shapes(cut_b, -big, big);
    its_transform(its_a, plane);
    its_transform(its_b, plane);
    out.a = TriangleMesh(its_a);
    out.b = TriangleMesh(its_b);
    for (auto &[its, on_a, unite] : ops_plane) {
        its_transform(its, plane, true);
        out.ops.push_back({TriangleMesh(std::move(its)), on_a, unite});
    }
    for (auto &[its, unused] : keys_plane) {
        its_transform(its, plane, true);
        out.keys.emplace_back(std::move(its));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// State, undo, preview

void GLGizmoAdvancedCut::curve_reset()
{
    m_curve_anchors.clear();
    m_curve_conns.clear();
    m_conn_editing         = false;
    m_conn_selected        = -1;
    m_conn_dragging        = false;
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
    m_curve_undo.push_back({m_curve_anchors, m_curve_drawing, m_curve_shape, m_curve_conns});
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
    to.push_back({m_curve_anchors, m_curve_drawing, m_curve_shape, m_curve_conns});
    m_curve_anchors = std::move(from.back().anchors);
    m_curve_drawing = from.back().drawing;
    m_curve_shape   = from.back().shape;
    m_curve_conns   = std::move(from.back().conns);
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
    CurveCutters cutters;
    if (!curve_build_cutters(cutters))
        return;
    std::vector<std::pair<size_t, TriangleMesh>> parts; // volume index, mesh in world coordinates
    const Transform3d inst_trafo = mo->instances[instance_idx]->get_matrix();
    for (size_t v = 0; v < mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part()) {
            parts.emplace_back(v, mo->volumes[v]->mesh());
            parts.back().second.transform(inst_trafo * mo->volumes[v]->get_matrix(), true);
        }

    std::thread([state = m_curve_preview, gen, cutters = std::move(cutters), parts = std::move(parts)]() mutable {
        // A newer outline makes this run obsolete: stop the boolean as soon as possible.
        const std::function<bool()> cancelled = [&state, gen]() {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->wanted != gen;
        };
        bool ok = true;
        // 3D connectors on the cutters
        for (const CurveCutters::Op &op : cutters.ops) {
            if (cancelled() || !ok)
                break;
            TriangleMesh &target = op.on_a ? cutters.a : cutters.b;
            target = TriangleMesh(mesh_boolean_op(target, op.mesh, op.unite ? "UNION" : "A_NOT_B", ok, cancelled));
        }
        if (cancelled())
            return;
        std::vector<CurvePreview::Piece> pieces;
        for (const auto &[volume, mesh] : parts) {
            if (cancelled())
                return;
            for (bool inside : {true, false}) {
                const indexed_triangle_set result = mesh_boolean(mesh, inside ? cutters.a : cutters.b, inside, ok, cancelled);
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

bool GLGizmoAdvancedCut::curve_pick_wall(const Vec2d &mouse_pos, double &t, double &z) const
{
    // hit the preview pieces, then keep the hit only if it lies on the cut wall (near the outline)
    const Camera &camera     = wxGetApp().plater()->get_camera();
    const Vec3d   camera_pos = camera.get_position();
    double        best_dist  = std::numeric_limits<double>::max();
    Vec3d         best_hit;
    bool          found = false;
    for (const auto &piece : m_curve_pieces) {
        if (m_conn_editing && piece->in_a == m_conn_show_b)
            continue; // only the part that is shown
        Vec3f pos, normal;
        if (piece->raycaster->unproject_on_mesh(mouse_pos, Transform3d::Identity(), camera, pos, normal)) {
            const double d = (camera_pos - pos.cast<double>()).squaredNorm();
            if (d < best_dist) {
                best_dist = d;
                best_hit  = pos.cast<double>();
                found     = true;
            }
        }
    }
    if (!found)
        return false;
    const Vec3d          local = curve_plane_matrix().inverse() * best_hit;
    const OutlineSampler sampler(curve_outline(false), m_shape_mode);
    if (!sampler.valid())
        return false;
    double dist = 0.0;
    t           = sampler.nearest(Vec2d(local.x(), local.y()), dist);
    z           = local.z();
    // connectors may sit a little inside the other part, so allow some distance
    return dist < std::max(1.0, m_radius * 0.05);
}

bool GLGizmoAdvancedCut::curve_delete_selected_connector()
{
    if (!m_curve_mode || !m_conn_editing || m_conn_selected < 0 || m_conn_selected >= int(m_curve_conns.size()))
        return false;
    curve_push_undo();
    m_curve_conns.erase(m_curve_conns.begin() + m_conn_selected);
    m_conn_selected = -1;
    curve_changed();
    m_parent.set_as_dirty();
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mouse

bool GLGizmoAdvancedCut::curve_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down)
{
    if (!m_curve_mode || m_connectors_editing || m_state != On)
        return false;

    // Connector editing: click the cut wall to add a connector there, click one to select it, drag it to
    // move it, right-click one to remove it.
    if (m_conn_editing) {
        auto conn_at = [this](double t, double z) {
            const OutlineSampler sampler(curve_outline(false), m_shape_mode);
            const double         tol = std::max(1.5, m_radius * 0.06);
            int                  best = -1;
            double               best_d = tol;
            if (sampler.valid())
                for (size_t i = 0; i < m_curve_conns.size(); ++i) {
                    Vec2d p, tg, q;
                    sampler.at(m_curve_conns[i].t, p, tg);
                    sampler.at(t, q, tg);
                    const double d = Vec3d(p.x() - q.x(), p.y() - q.y(), m_curve_conns[i].z - z).norm();
                    if (d < best_d) {
                        best_d = d;
                        best   = int(i);
                    }
                }
            return best;
        };
        double t = 0.0, z = 0.0;
        switch (action) {
        case SLAGizmoEventType::LeftDown: {
            if (m_hover_id != -1 && m_hover_id != CURVE_PLATE_MOVE_ID)
                return false;
            if (!curve_pick_wall(mouse_position, t, z))
                return false; // not on the wall: let the view rotate
            curve_push_undo();
            if (const int idx = conn_at(t, z); idx >= 0) {
                m_conn_selected = idx;
                m_conn          = m_curve_conns[idx].params; // the panel edits the selected one
            } else {
                m_curve_conns.push_back({t, z, m_conn});
                m_conn_selected = int(m_curve_conns.size()) - 1;
            }
            m_conn_dragging = true;
            m_parent.set_as_dirty();
            return true;
        }
        case SLAGizmoEventType::Dragging:
            if (!m_conn_dragging)
                return false;
            if (m_conn_selected >= 0 && m_conn_selected < int(m_curve_conns.size()) && curve_pick_wall(mouse_position, t, z)) {
                m_curve_conns[m_conn_selected].t = t;
                m_curve_conns[m_conn_selected].z = z;
                m_parent.set_as_dirty();
            }
            return true;
        case SLAGizmoEventType::LeftUp:
            if (!m_conn_dragging)
                return false;
            m_conn_dragging = false;
            curve_changed();
            m_parent.set_as_dirty();
            return true;
        case SLAGizmoEventType::RightDown:
            if (curve_pick_wall(mouse_position, t, z))
                if (const int idx = conn_at(t, z); idx >= 0) {
                    m_conn_selected = idx;
                    curve_delete_selected_connector();
                }
            return true;
        case SLAGizmoEventType::RightUp: return true;
        default: return false;
        }
    }

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
        if (m_conn_editing ? piece->in_a == m_conn_show_b : (piece->in_a ? !m_keep_upper : !m_keep_lower))
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
    // connectors as 3D shapes (selected one highlighted); separate pieces in their own colour
    if (!m_curve_conns.empty()) {
        CurveCutters shapes;
        const OutlineSampler sampler(curve_outline(false), m_shape_mode);
        if (sampler.valid()) {
            const BoundingBoxf3 tbb  = transformed_bounding_box(m_plane_center, m_rotate_matrix);
            const double        z_lo = tbb.defined ? tbb.min.z() : -m_radius, z_hi = tbb.defined ? tbb.max.z() : m_radius;
            for (size_t i = 0; i < m_curve_conns.size(); ++i) {
                const CurveConnector  &conn = m_curve_conns[i];
                const CurveConnParams &c    = conn.params;
                ConnFrame f;
                sampler.at(conn.t, f.p, f.tangent);
                f.dir = Vec2d(f.tangent.y(), -f.tangent.x());
                if (c.on_part_b && !c.separate())
                    f.dir = -f.dir;
                const double z = std::clamp(conn.z, z_lo, z_hi);
                indexed_triangle_set its;
                switch (c.type) {
                case ConnType::Dovetail: {
                    const double run = c.closed_end ? (z_hi - z_lo) * std::clamp(c.length, 5.0, 100.0) / 100.0 : (z_hi - z_lo);
                    its = c.closed_end && c.reverse ? dovetail_solid(f, z_lo, z_lo + run, c, 0.0) : dovetail_solid(f, z_hi - run, z_hi, c, 0.0);
                    break;
                }
                case ConnType::Pin: its = pin_solid(f, z, c, 0.0); break;
                case ConnType::Snap: its = snap_solid(f, z, c, 0.0); break;
                case ConnType::Dowel: its = dowel_solid(f, z, c, 0.0); break;
                case ConnType::Bowtie: its = extrude_shapes(union_ex(Polygons{bowtie_profile(f, c, 0.0)}), z_lo, z_hi); break;
                }
                its_transform(its, plane, true);
                GLModel model;
                model.init_from(its);
                const ColorRGBA color = int(i) == m_conn_selected ? ColorRGBA::GREEN() : c.separate() ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE();
                glsafe(::glEnable(GL_DEPTH_TEST));
                render_glmodel(model, color.get_data(), camera.get_view_matrix(), camera.get_projection_matrix());
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
        m_curve_conns.clear();
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

    ImGui::Separator();
    render_curve_connectors_panel();

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

void GLGizmoAdvancedCut::render_curve_connectors_panel()
{
    if (!m_conn_editing) {
        m_imgui->text(wxString::Format(_L("Connectors: %d"), int(m_curve_conns.size())));
        ImGui::SameLine();
        m_imgui->disabled_begin(!curve_preview_visible());
        if (m_imgui->button(m_curve_conns.empty() ? _L("Add connectors") : _L("Edit connectors"))) {
            m_conn_editing  = true;
            m_conn_selected = -1;
        }
        m_imgui->disabled_end();
        return;
    }

    m_imgui->text(_L("Editing connectors:"));
    m_imgui->text(" - " + _L("Click on the cut wall to add one; rotate the view to see every side"));
    m_imgui->text(" - " + _L("Click one to select and edit it, drag it to move it"));
    m_imgui->text(" - " + _L("Right-click one or press Delete to remove it"));
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Show") + ":");
    ImGui::SameLine();
    if (ImGui::RadioButton(_u8L("Part A").c_str(), !m_conn_show_b))
        m_conn_show_b = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(_u8L("Part B").c_str(), m_conn_show_b))
        m_conn_show_b = true;
    ImGui::Separator();

    // the settings of the selected connector, or the ones new connectors get
    const bool       has_sel = m_conn_selected >= 0 && m_conn_selected < int(m_curve_conns.size());
    CurveConnParams &c       = has_sel ? m_curve_conns[m_conn_selected].params : m_conn;
    m_imgui->text(has_sel ? wxString::Format(_L("Connector %d"), m_conn_selected + 1) : _L("New connectors"));
    bool       changed = false;
    const auto before  = c;
    const std::vector<std::string> types = {_u8L("Dovetail slide"), _u8L("Pin"), _u8L("Snap-fit barb"), _u8L("Dowel (separate piece)"),
                                            _u8L("Bowtie key (separate piece)")};
    size_t type_idx = size_t(c.type);
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    if (render_combo(_u8L("Type"), types, type_idx, m_label_width, m_imgui->scaled(12.f))) {
        c.type  = ConnType(type_idx);
        changed = true;
    }
    ImGuiWrapper::pop_combo_style();

    // slider + exact value, like the planar connectors
    auto edit = [&](const wxString &label, double &value, double min_value, double max_value, const char *fmt = "%.2f") {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(m_label_width);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        float v = float(value);
        if (m_imgui->bbl_slider_float_style(("##conn_" + label.ToStdString()).c_str(), &v, float(min_value), float(max_value), fmt) &&
            std::abs(v - value) > EPSILON) {
            value   = double(v);
            changed = true;
        }
        ImGui::PopItemWidth();
    };
    switch (c.type) {
    case ConnType::Dovetail:
        edit(_L("Width") + " (mm)", c.width, 1.0, 50.0);
        edit(_L("Depth") + " (mm)", c.depth, 0.5, 30.0);
        edit(_L("Angle") + " (deg)", c.angle, 0.0, 45.0, "%.1f");
        changed |= m_imgui->bbl_checkbox(_L("Closed end (stops sliding)"), c.closed_end);
        if (c.closed_end)
            edit(_L("Length") + " (%)", c.length, 5.0, 100.0, "%.0f");
        break;
    case ConnType::Pin:
        edit(_L("Diameter") + " (mm)", c.pin_diameter, 0.5, 30.0);
        edit(_L("Length") + " (mm)", c.pin_length, 0.5, 50.0);
        break;
    case ConnType::Snap:
        edit(_L("Width") + " (mm)", c.width, 1.0, 50.0);
        edit(_L("Depth") + " (mm)", c.depth, 0.2, 10.0);
        edit(_L("Ramp length") + " (mm)", c.snap_height, 0.5, 50.0);
        break;
    case ConnType::Dowel:
        edit(_L("Diameter") + " (mm)", c.pin_diameter, 0.5, 30.0);
        edit(_L("Length") + " (mm)", c.pin_length, 1.0, 100.0);
        break;
    case ConnType::Bowtie:
        edit(_L("Neck width") + " (mm)", c.width, 1.0, 50.0);
        edit(_L("Depth per side") + " (mm)", c.depth, 0.5, 30.0);
        edit(_L("Angle") + " (deg)", c.angle, 0.0, 45.0, "%.1f");
        break;
    }
    edit(_L("Clearance") + " (mm)", c.tolerance, 0.0, 1.0, "%.2f");
    if (!c.separate())
        changed |= m_imgui->bbl_checkbox(_L("On part B"), c.on_part_b);
    if (c.type == ConnType::Snap || (c.type == ConnType::Dovetail && c.closed_end)) {
        if (!c.separate())
            ImGui::SameLine();
        changed |= m_imgui->bbl_checkbox(_L("Slide in from the other side"), c.reverse);
    }

    if (changed) {
        // one undo step per change; new connectors take over the last used settings
        const auto after = c;
        if (has_sel)
            m_curve_conns[m_conn_selected].params = before;
        curve_push_undo();
        if (has_sel)
            m_curve_conns[m_conn_selected].params = after;
        m_conn = after;
        curve_changed();
    }

    m_imgui->disabled_begin(!has_sel);
    if (m_imgui->button(_L("Remove")))
        curve_delete_selected_connector();
    m_imgui->disabled_end();
    ImGui::SameLine();
    m_imgui->disabled_begin(m_curve_conns.empty());
    if (m_imgui->button(_L("Remove all"))) {
        curve_push_undo();
        m_curve_conns.clear();
        m_conn_selected = -1;
        curve_changed();
    }
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Done"))) {
        m_conn_editing  = false;
        m_conn_selected = -1;
    }
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

    // separate connector pieces (dowels, bowtie keys) become their own objects, where they belong
    CurveCutters cutters;
    if (curve_build_cutters(cutters))
        for (size_t i = 0; i < cutters.keys.size(); ++i) {
            TriangleMesh key    = std::move(cutters.keys[i]);
            const Vec3d  center = key.bounding_box().center();
            key.translate(-center.cast<float>());
            ModelObject *obj = scratch.add_object();
            obj->name        = mo->name + " - " + _u8L("connector") + " " + std::to_string(i + 1);
            obj->add_volume(std::move(key));
            obj->add_instance()->set_offset(center);
            obj->invalidate_bounding_box();
            results.push_back(obj);
        }

    m_parent.reset_all_gizmos();
    Plater::TakeSnapshot snapshot(plater, m_shape_mode ? "Cut by shape" : "Cut by curve");
    plater->apply_cut_object_to_model(size_t(object_idx), results);
    curve_reset();
}

} // namespace GUI
} // namespace Slic3r
