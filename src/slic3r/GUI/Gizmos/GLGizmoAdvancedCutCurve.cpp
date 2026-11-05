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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <tuple>

#include <GL/glew.h>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Widgets/ProgressDialog.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/Win10ModelRepair.hpp"

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
    bool               simplified{false}; // made on a simplified copy of the model
    std::string        error;             // why it failed, for the user
    std::vector<int>   skipped;           // connectors that could not be added
    std::vector<Piece> pieces;
    // what the worker is doing now, for the panel
    bool                                  running{false};
    std::string                           stage;
    std::chrono::steady_clock::time_point started;

    // Cleaned up (and for big models simplified) copies of the model meshes, in volume coordinates.
    // The entry keeps the source mesh alive, so its address identifies it.
    struct CacheEntry
    {
        std::shared_ptr<const TriangleMesh>         src;
        std::shared_ptr<const indexed_triangle_set> prepared;
        bool                                        simplified{false};
    };
    std::mutex              cache_mutex;
    std::vector<CacheEntry> cache;
};

struct GLGizmoAdvancedCut::CurvePiece
{
    size_t                         volume{0};
    bool                           in_a{true};
    bool                           orig_a{true}; // side it was computed on, before the user moved it
    std::shared_ptr<TriangleMesh>  mesh;      // world coordinates
    std::shared_ptr<MeshRaycaster> raycaster; // references 'mesh'
    std::shared_ptr<GLModel>       model;
};

namespace {

// Same id as in GLGizmoAdvancedCut.cpp
constexpr int CURVE_PLATE_MOVE_ID = 5;

const ColorRGBA CURVE_PART_A_COLOR = ColorRGBA::CYAN();
const ColorRGBA CURVE_PART_B_COLOR = ColorRGBA::MAGENTA();

// Model meshes bigger than this are simplified for the preview; the cut itself uses the full mesh.
constexpr size_t PREVIEW_SIMPLIFY_ABOVE = 300000;
constexpr size_t PREVIEW_TRIANGLES      = 200000;

struct CurveCancelled {}; // thrown from the simplification when the run became obsolete

// Every boolean runs alone: two calculations at once only compete for the CPU, and the backend is not
// known to be safe to run twice in parallel.
std::mutex &curve_boolean_mutex()
{
    static std::mutex mutex;
    return mutex;
}

// Closed, consistently oriented 2-manifold: every directed edge exactly once, together with its reverse.
// The boolean backend can corrupt memory on anything else, so cutters are checked before use.
bool its_is_closed_manifold(const indexed_triangle_set &its)
{
    if (its.indices.empty())
        return false;
    std::vector<uint64_t> edges;
    edges.reserve(its.indices.size() * 3);
    for (const stl_triangle_vertex_indices &f : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = f[i], b = f[(i + 1) % 3];
            if (a == b || a < 0 || b < 0 || a >= int(its.vertices.size()) || b >= int(its.vertices.size()))
                return false;
            edges.push_back((uint64_t(uint32_t(a)) << 32) | uint64_t(uint32_t(b)));
        }
    std::sort(edges.begin(), edges.end());
    if (std::adjacent_find(edges.begin(), edges.end()) != edges.end())
        return false; // an edge used twice in the same direction: flipped face or non-manifold edge
    for (const uint64_t e : edges) {
        const uint64_t rev = (e << 32) | (e >> 32);
        if (!std::binary_search(edges.begin(), edges.end(), rev))
            return false; // open edge
    }
    return true;
}

struct RepairCounts
{
    int removed{0}; // degenerate and duplicate faces
    int flipped{0}; // faces turned to match their neighbours or the outside
};

// Turns the faces of every connected part to agree with their neighbours, then the whole part outwards.
int its_orient_consistently(indexed_triangle_set &its)
{
    const size_t n = its.indices.size();
    if (n == 0)
        return 0;
    const std::vector<Vec3i> neighbors = its_face_neighbors(its);
    std::vector<char>        visited(n, 0);
    std::vector<size_t>      stack, faces;
    int                      flipped = 0;
    auto flip = [&its](size_t f) { std::swap(its.indices[f][1], its.indices[f][2]); };
    for (size_t seed = 0; seed < n; ++seed) {
        if (visited[seed])
            continue;
        faces.clear();
        visited[seed] = 1;
        stack.push_back(seed);
        while (!stack.empty()) {
            const size_t f = stack.back();
            stack.pop_back();
            faces.push_back(f);
            for (int k = 0; k < 3; ++k) {
                const int g = neighbors[f][k];
                if (g < 0 || size_t(g) >= n || visited[g])
                    continue;
                // find the shared edge and compare its direction in both faces
                const stl_triangle_vertex_indices &ff = its.indices[f];
                const stl_triangle_vertex_indices &gg = its.indices[g];
                bool same_dir = false, shared = false;
                for (int i = 0; i < 3 && !shared; ++i)
                    for (int j = 0; j < 3 && !shared; ++j) {
                        const int a = ff[i], b = ff[(i + 1) % 3];
                        if (gg[j] == a && gg[(j + 1) % 3] == b) {
                            shared   = true;
                            same_dir = true;
                        } else if (gg[j] == b && gg[(j + 1) % 3] == a)
                            shared = true;
                    }
                if (!shared)
                    continue;
                if (same_dir) {
                    flip(size_t(g));
                    ++flipped;
                }
                visited[g] = 1;
                stack.push_back(size_t(g));
            }
        }
        // the part as a whole faces outwards
        double volume = 0.0;
        for (size_t f : faces) {
            const Vec3d a = its.vertices[its.indices[f][0]].cast<double>(), b = its.vertices[its.indices[f][1]].cast<double>(),
                        c = its.vertices[its.indices[f][2]].cast<double>();
            volume += a.dot(b.cross(c));
        }
        if (volume < 0.0) {
            for (size_t f : faces)
                flip(f);
            flipped += int(faces.size());
        }
    }
    return flipped;
}

// Cheap clean-up: merge coincident vertices, drop degenerate and duplicate faces, fix face orientation.
RepairCounts repair_its(indexed_triangle_set &its)
{
    RepairCounts counts;
    its_merge_vertices(its);
    counts.removed += its_remove_degenerate_faces(its);
    {
        // duplicate faces (same three vertices, either orientation)
        std::vector<std::pair<std::array<int, 3>, size_t>> keys;
        keys.reserve(its.indices.size());
        for (size_t f = 0; f < its.indices.size(); ++f) {
            std::array<int, 3> k = {its.indices[f][0], its.indices[f][1], its.indices[f][2]};
            std::sort(k.begin(), k.end());
            keys.emplace_back(k, f);
        }
        std::sort(keys.begin(), keys.end());
        std::vector<char> drop(its.indices.size(), 0);
        for (size_t i = 1; i < keys.size(); ++i)
            if (keys[i].first == keys[i - 1].first)
                drop[keys[i].second] = 1;
        size_t out = 0;
        for (size_t f = 0; f < its.indices.size(); ++f)
            if (!drop[f])
                its.indices[out++] = its.indices[f];
        counts.removed += int(its.indices.size() - out);
        its.indices.resize(out);
    }
    its_compactify_vertices(its);
    counts.flipped = its_orient_consistently(its);
    return counts;
}

// Closed solid from 2D shapes (plane frame, mm): caps at z = z0 / z1 plus side walls.
// Every shape is built on its own, so shapes touching in a point do not share (non-manifold) edges.
indexed_triangle_set extrude_shapes(const ExPolygons &shapes, double z0, double z1)
{
    indexed_triangle_set result;
    for (const ExPolygon &shape : shapes) {
        std::vector<Vec3d> soup;
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
        its_merge(result, its);
    }
    return result;
}

// 2D shapes cleaned for extrusion: tiny segments removed and a hair shrunk, so contours touching in a
// point (a path crossing itself, a connector touching the outline) come apart.
ExPolygons clean_shapes(const ExPolygons &shapes, double tolerance)
{
    return offset_ex(expolygons_simplify(shapes, scale_(tolerance)), -float(scale_(tolerance)));
}

// Closed solid from 2D shapes that the boolean backend can take, or an empty mesh. Tries a little more
// simplification when the first extrusion is not a clean closed surface.
indexed_triangle_set extrude_valid(const ExPolygons &shapes, double z0, double z1)
{
    for (double tolerance : {0.001, 0.01, 0.05}) {
        const ExPolygons clean = clean_shapes(shapes, tolerance);
        if (clean.empty())
            break;
        indexed_triangle_set its = extrude_shapes(clean, z0, z1);
        if (its_is_closed_manifold(its))
            return its;
    }
    return {};
}

// A op B ("INTERSECTION", "A_NOT_B" or "UNION") with the default backend, CGAL as fallback.
// 'ok' is set to false when both failed. 'cancel' (optional) is polled by the boolean backend; a
// cancelled run returns an empty mesh.
indexed_triangle_set mesh_boolean_op(const TriangleMesh &a, const TriangleMesh &b, const std::string &op, bool &ok,
                                     const std::function<bool()> &cancel = nullptr)
{
    std::vector<TriangleMesh> results;
    bool failed = false;
    if (a.its.indices.empty() || b.its.indices.empty()) {
        ok = false;
        return {};
    }
    try {
        MeshBoolean::mcut::make_boolean(a, b, results, op, cancel, nullptr, [&failed]() { failed = true; });
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "Curve cut: mesh boolean threw: " << e.what();
        results.clear();
        failed = true;
    }
    if (cancel && cancel())
        return {};
    indexed_triangle_set its;
    if (!results.empty()) {
        for (const TriangleMesh &r : results)
            its_merge(its, r.its);
    } else if (failed) {
        // CGAL cannot be cancelled and takes minutes on huge meshes: report the failure instead
        if (a.its.indices.size() + b.its.indices.size() > 600000) {
            ok = false;
            return {};
        }
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
        } catch (...) {
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

// A loose pin is a dowel reaching 'pin_length' into each part.
GLGizmoAdvancedCut::CurveConnParams loose_pin(const GLGizmoAdvancedCut::CurveConnParams &c)
{
    GLGizmoAdvancedCut::CurveConnParams out = c;
    out.pin_length                          = 2.0 * c.pin_length;
    return out;
}

// Loose snap-fit clip: a barb on each side of the wall, both catching at the same height, joined by a
// core across the wall. Profile in (v into B, w along the plane normal), extruded along the tangent.
indexed_triangle_set double_snap_solid(const ConnFrame &f, double z, const GLGizmoAdvancedCut::CurveConnParams &c, double extra)
{
    const double hu = 0.5 * c.width + extra, h0 = z - 0.5 * c.snap_height - extra, h1 = z + 0.5 * c.snap_height + extra;
    const double tip = c.depth + extra, core = 0.4 * c.depth + extra;
    const double cw = c.reverse ? h1 : h0, rw = c.reverse ? h0 : h1;
    const double w1 = cw + (rw - cw) * (tip - core) / (tip + core); // where the ramps meet the core
    const std::array<Vec2d, 6> q = {Vec2d(-tip, cw), Vec2d(tip, cw), Vec2d(core, w1), Vec2d(core, rw), Vec2d(-core, rw), Vec2d(-core, w1)};
    Polygon poly;
    for (const Vec2d &v : q)
        poly.points.emplace_back(scale_(v.x()), scale_(v.y()));
    if (!poly.is_counter_clockwise())
        poly.reverse();
    indexed_triangle_set its = extrude_shapes(union_ex(Polygons{poly}), -hu, hu);
    // extrusion (x, y, z) = (v, w, u) -> plane frame
    Transform3d m = Transform3d::Identity();
    m.linear().col(0) = Vec3d(f.dir.x(), f.dir.y(), 0.0);
    m.linear().col(1) = Vec3d::UnitZ();
    m.linear().col(2) = Vec3d(f.tangent.x(), f.tangent.y(), 0.0);
    m.translation()   = Vec3d(f.p.x(), f.p.y(), 0.0);
    its_transform(its, m, true);
    orient_outwards(its);
    return its;
}

// The connector as it looks on the finished parts, for the 3D view.
indexed_triangle_set conn_display_solid(const ConnFrame &f, double z, const GLGizmoAdvancedCut::CurveConnParams &c, double z_lo, double z_hi)
{
    using ConnType = GLGizmoAdvancedCut::ConnType;
    switch (c.type) {
    case ConnType::Dovetail: {
        const double run = c.closed_end ? (z_hi - z_lo) * std::clamp(c.length, 5.0, 100.0) / 100.0 : (z_hi - z_lo);
        const double z0  = c.closed_end && c.reverse ? z_lo : z_hi - run;
        // a loose dovetail is a double dovetail key, like the bowtie
        return c.loose ? extrude_shapes(union_ex(Polygons{bowtie_profile(f, c, 0.0)}), z0, z0 + run) : dovetail_solid(f, z0, z0 + run, c, 0.0);
    }
    case ConnType::Pin: return c.loose ? dowel_solid(f, z, loose_pin(c), 0.0) : pin_solid(f, z, c, 0.0);
    case ConnType::Snap: return c.loose ? double_snap_solid(f, z, c, 0.0) : snap_solid(f, z, c, 0.0);
    case ConnType::Dowel: return dowel_solid(f, z, c, 0.0);
    case ConnType::Bowtie: return extrude_shapes(union_ex(Polygons{bowtie_profile(f, c, 0.0)}), z_lo, z_hi);
    }
    return {};
}

// One model part to split: its mesh (shared, never modified) and where it sits in the world.
struct CurvePart
{
    size_t                              volume{0};
    std::shared_ptr<const TriangleMesh> mesh;
    Transform3d                         trafo{Transform3d::Identity()};
    bool                                has_issues{false}; // open / non-manifold edges or reversed faces
};

struct CurveJobResult
{
    bool                                                ok{true};
    bool                                                simplified{false};
    std::string                                         error;
    std::vector<int>                                    skipped; // connectors that could not be added
    std::vector<GLGizmoAdvancedCut::CurvePreview::Piece> pieces;
};

// Splits every part with the cutters. Runs on a worker thread and only touches its own copies and the
// shared preview cache. 'cache' is null for the real cut, which uses the full-resolution meshes.
// Returns false when cancelled.
bool run_curve_job(GLGizmoAdvancedCut::CurveCutters &cutters, const std::vector<CurvePart> &parts, GLGizmoAdvancedCut::CurvePreview *cache,
                   const std::function<bool()> &cancelled, const std::function<void(const std::string &)> &stage, CurveJobResult &out)
{
    out.skipped = cutters.bad_conns;
    stage(_u8L("Waiting for the previous calculation to stop..."));
    std::unique_lock<std::mutex> lock(curve_boolean_mutex(), std::defer_lock);
    while (!lock.try_lock()) {
        if (cancelled())
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // the model, cleaned up (and simplified for the preview of big models)
    std::vector<std::pair<size_t, TriangleMesh>> meshes;
    bool any_issues = false;
    for (const CurvePart &part : parts) {
        if (cancelled())
            return false;
        any_issues |= part.has_issues;
        std::shared_ptr<const indexed_triangle_set> prepared;
        bool                                        simplified = false;
        if (cache) {
            std::lock_guard<std::mutex> cache_lock(cache->cache_mutex);
            for (const auto &entry : cache->cache)
                if (entry.src == part.mesh) {
                    prepared   = entry.prepared;
                    simplified = entry.simplified;
                }
        }
        if (!prepared) {
            const size_t count    = part.mesh->its.indices.size();
            const bool   simplify = cache != nullptr && count > PREVIEW_SIMPLIFY_ABOVE;
            stage(simplify ? (boost::format(_u8L("Simplifying the model for the preview (%1% triangles)...")) % count).str() :
                             _u8L("Checking the model..."));
            indexed_triangle_set its = part.mesh->its;
            repair_its(its);
            if (simplify) {
                try {
                    its_quadric_edge_collapse(
                        its, uint32_t(PREVIEW_TRIANGLES), nullptr,
                        [&cancelled]() {
                            if (cancelled())
                                throw CurveCancelled();
                        },
                        [&stage](int percent) { stage((boost::format(_u8L("Simplifying the model for the preview: %1%%%")) % percent).str()); });
                } catch (const CurveCancelled &) {
                    return false;
                }
                repair_its(its);
            }
            prepared   = std::make_shared<const indexed_triangle_set>(std::move(its));
            simplified = simplify;
            if (cache) {
                std::lock_guard<std::mutex> cache_lock(cache->cache_mutex);
                cache->cache.push_back({part.mesh, prepared, simplified});
                if (cache->cache.size() > 8)
                    cache->cache.erase(cache->cache.begin());
            }
        }
        out.simplified |= simplified;
        indexed_triangle_set world = *prepared;
        its_transform(world, part.trafo, true);
        meshes.emplace_back(part.volume, TriangleMesh(std::move(world)));
    }

    // 3D connectors on the cutters, all operations of one connector or none of them
    for (size_t i = 0; i < cutters.ops.size();) {
        if (cancelled())
            return false;
        const int conn = cutters.ops[i].conn;
        size_t    end  = i;
        while (end < cutters.ops.size() && cutters.ops[end].conn == conn)
            ++end;
        stage((boost::format(_u8L("Adding connector %1%...")) % (conn + 1)).str());
        TriangleMesh a = cutters.a, b = cutters.b;
        bool         good = true;
        for (size_t k = i; k < end && good; ++k) {
            const GLGizmoAdvancedCut::CurveCutters::Op &op = cutters.ops[k];
            TriangleMesh &target = op.on_a ? a : b;
            bool ok = true;
            indexed_triangle_set r = mesh_boolean_op(target, op.mesh, op.unite ? "UNION" : "A_NOT_B", ok, cancelled);
            if (cancelled())
                return false;
            if (ok && !its_is_closed_manifold(r))
                repair_its(r); // usually only a few faces turned the wrong way
            good = ok && its_is_closed_manifold(r);
            if (good)
                target = TriangleMesh(std::move(r));
        }
        if (good) {
            cutters.a = std::move(a);
            cutters.b = std::move(b);
        } else if (std::find(out.skipped.begin(), out.skipped.end(), conn) == out.skipped.end())
            out.skipped.push_back(conn);
        i = end;
    }

    // split every part
    for (size_t i = 0; i < meshes.size(); ++i) {
        for (bool inside : {true, false}) {
            if (cancelled())
                return false;
            stage(meshes.size() > 1 ? (boost::format(_u8L("Cutting part %1% of %2%...")) % (i + 1) % meshes.size()).str() : _u8L("Cutting the model..."));
            bool ok = true;
            const indexed_triangle_set result = mesh_boolean(meshes[i].second, inside ? cutters.a : cutters.b, inside, ok, cancelled);
            if (cancelled())
                return false;
            if (!ok) {
                out.ok = false;
                continue;
            }
            for (indexed_triangle_set &piece : its_split(result))
                out.pieces.push_back({meshes[i].first, inside, std::move(piece)});
        }
    }
    if (!out.ok)
        out.error = any_issues ? _u8L("The model has open or non-manifold edges or reversed faces. Click \"Repair model\" and try again.") :
                                 _u8L("The cut could not be calculated here. Move the line or the connectors a little and try again.");
    return true;
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
    // Points on top of each other (two clicks in one spot, a handle pulled back) give zero-length
    // segments whose direction is undefined: drop them.
    std::vector<Vec2d> clean;
    clean.reserve(out.size());
    for (const Vec2d &p : out)
        if (std::isfinite(p.x()) && std::isfinite(p.y()) && (clean.empty() || (p - clean.back()).norm() > 1e-4))
            clean.push_back(p);
    return clean;
}

bool GLGizmoAdvancedCut::curve_build_cutters(CurveCutters &out) const
{
    out = CurveCutters();
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
    for (const Vec2d &p : outline) {
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            return false;
        poly.points.emplace_back(scale_(p.x()), scale_(p.y()));
    }
    // union_ex fixes orientation and self intersections of a hand drawn path
    const ExPolygons base = union_ex(Polygons{poly});
    if (base.empty()) {
        out.error = _u8L("The outline does not enclose anything. Draw it across the model.");
        return false;
    }

    // Connectors, each with its own settings. Part A lies left of an open path and inside a shape, so
    // "into B" is the right-hand normal of the outline.
    //   owned by A:  cutter a += solid,  cutter b += hole
    //   owned by B:  cutter a -= hole,   cutter b -= solid
    //   separate:    cutter a -= hole,   cutter b += hole, plus the key as its own piece
    Polygons add_a, sub_a, add_b, sub_b; // full-height shapes, done in 2D with the outline
    struct PlaneOp
    {
        indexed_triangle_set its;
        bool                 on_a, unite;
        int                  conn;
    };
    std::vector<PlaneOp>                            ops_plane;
    struct PlaneKey
    {
        int                  conn;
        indexed_triangle_set its; // key in the plane frame
        Vec3d                up;  // print orientation in the plane frame
    };
    std::vector<PlaneKey> keys_plane;
    const BoundingBoxf3  tbb  = transformed_bounding_box(m_plane_center, m_rotate_matrix); // plane frame
    const double         z_lo = tbb.defined ? tbb.min.z() : -m_radius, z_hi = tbb.defined ? tbb.max.z() : m_radius;
    const OutlineSampler sampler(pts, m_shape_mode);
    if (sampler.valid())
        for (int ci = 0; ci < int(m_curve_conns.size()); ++ci) {
            const CurveConnector  &conn = m_curve_conns[ci];
            const CurveConnParams &c    = conn.params;
            ConnFrame f;
            sampler.at(conn.t, f.p, f.tangent);
            f.dir = Vec2d(f.tangent.y(), -f.tangent.x());
            const bool owner_b = c.on_part_b && !c.separate();
            if (owner_b)
                f.dir = -f.dir; // grows out of part B into part A
            const double tol = c.tolerance;
            const double z   = std::clamp(conn.z, z_lo, z_hi);
            // how a separate piece is printed: dowels and pins standing, the clip and the keys on their profile
            const Vec3d key_up = (c.type == ConnType::Dowel || c.type == ConnType::Pin) ? Vec3d(f.dir.x(), f.dir.y(), 0.0) :
                                 c.type == ConnType::Snap                               ? Vec3d(f.tangent.x(), f.tangent.y(), 0.0) :
                                                                                          Vec3d::UnitZ();
            auto add_3d = [&](indexed_triangle_set solid, indexed_triangle_set hole) {
                // a broken connector shape is left out instead of breaking the whole cut
                if (!its_is_closed_manifold(solid) || !its_is_closed_manifold(hole)) {
                    out.bad_conns.push_back(ci);
                    return;
                }
                if (c.separate()) {
                    ops_plane.push_back({hole, true, false, ci});
                    ops_plane.push_back({std::move(hole), false, true, ci});
                    keys_plane.push_back({ci, std::move(solid), key_up});
                } else if (!owner_b) {
                    ops_plane.push_back({std::move(solid), true, true, ci});
                    ops_plane.push_back({std::move(hole), false, true, ci});
                } else {
                    ops_plane.push_back({std::move(hole), true, false, ci});
                    ops_plane.push_back({std::move(solid), false, false, ci});
                }
            };
            // a key as tall as the object that slides in along the wall, with a slot in both parts
            auto add_full_key = [&](const Polygon &key, const Polygon &slot) {
                indexed_triangle_set solid = extrude_valid(union_ex(Polygons{key}), z_lo, z_hi);
                if (solid.indices.empty()) {
                    out.bad_conns.push_back(ci);
                    return;
                }
                sub_a.push_back(slot);
                add_b.push_back(slot);
                keys_plane.push_back({ci, std::move(solid), key_up});
            };
            switch (c.type) {
            case ConnType::Dovetail:
                if (c.loose) {
                    // double dovetail key across the wall
                    if (!c.closed_end) {
                        add_full_key(bowtie_profile(f, c, 0.0), bowtie_profile(f, c, tol));
                    } else {
                        const double run = (z_hi - z_lo) * std::clamp(c.length, 5.0, 100.0) / 100.0;
                        const double z0 = c.reverse ? z_lo : z_hi - run, z1 = z0 + run;
                        add_3d(extrude_valid(union_ex(Polygons{bowtie_profile(f, c, 0.0)}), z0, z1),
                               extrude_valid(union_ex(Polygons{bowtie_profile(f, c, tol)}), c.reverse ? -big : z0 - tol, c.reverse ? z1 + tol : big));
                    }
                } else if (!c.closed_end) {
                    (owner_b ? sub_b : add_a).push_back(dovetail_profile(f, c, 0.0));
                    (owner_b ? sub_a : add_b).push_back(dovetail_profile(f, c, tol));
                } else {
                    // closed at one end: the slide stops there and the part stays put
                    const double run = (z_hi - z_lo) * std::clamp(c.length, 5.0, 100.0) / 100.0;
                    const double z0 = c.reverse ? -big : z_hi - run, z1 = c.reverse ? z_lo + run : big;
                    add_3d(dovetail_solid(f, z0, z1, c, 0.0), dovetail_solid(f, c.reverse ? z0 : z0 - tol, c.reverse ? z1 + tol : z1, c, tol));
                }
                break;
            case ConnType::Pin:
                if (c.loose)
                    add_3d(dowel_solid(f, z, loose_pin(c), 0.0), dowel_solid(f, z, loose_pin(c), tol));
                else
                    add_3d(pin_solid(f, z, c, 0.0), pin_solid(f, z, c, tol));
                break;
            case ConnType::Snap:
                if (c.loose)
                    add_3d(double_snap_solid(f, z, c, 0.0), double_snap_solid(f, z, c, tol));
                else
                    add_3d(snap_solid(f, z, c, 0.0), snap_solid(f, z, c, tol));
                break;
            case ConnType::Dowel: add_3d(dowel_solid(f, z, c, 0.0), dowel_solid(f, z, c, tol)); break;
            case ConnType::Bowtie: add_full_key(bowtie_profile(f, c, 0.0), bowtie_profile(f, c, tol)); break;
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
    const Transform3d    plane = curve_plane_matrix();
    indexed_triangle_set its_a = extrude_valid(cut_a, -big, big), its_b = extrude_valid(cut_b, -big, big);
    if (its_a.indices.empty() || its_b.indices.empty()) {
        // never hand a broken cutter to the boolean backend: it can crash on it
        out.error = _u8L("This outline cannot be used: it crosses itself too tightly. Smooth it or remove a point.");
        return false;
    }
    its_transform(its_a, plane);
    its_transform(its_b, plane);
    out.a = TriangleMesh(std::move(its_a));
    out.b = TriangleMesh(std::move(its_b));
    for (PlaneOp &op : ops_plane) {
        its_transform(op.its, plane, true);
        out.ops.push_back({TriangleMesh(std::move(op.its)), op.on_a, op.unite, op.conn});
    }
    for (PlaneKey &k : keys_plane) {
        its_transform(k.its, plane, true);
        out.keys.push_back({k.conn, TriangleMesh(std::move(k.its)), (plane.linear() * k.up).normalized()});
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
    m_conn_hover.reset();
    m_curve_preview_failed = false;
    m_curve_repair_msg.clear();
    if (m_curve_preview) {
        // drop the simplified model copies; a running worker keeps what it already uses
        std::lock_guard<std::mutex> lock(m_curve_preview->cache_mutex);
        m_curve_preview->cache.clear();
    }
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
    return m_curve_mode && !m_curve_pieces.empty() && m_curve_preview_shown == m_curve_generation && !m_curve_preview_failed;
}

bool GLGizmoAdvancedCut::curve_preview_visible() const
{
    return m_curve_mode && !m_curve_pieces.empty();
}

namespace {
// The model parts of the selected instance, shared (not copied) with the worker.
std::vector<CurvePart> curve_collect_parts(const ModelObject *mo, int instance_idx)
{
    std::vector<CurvePart> parts;
    const Transform3d inst_trafo = mo->instances[instance_idx]->get_matrix();
    for (size_t v = 0; v < mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part()) {
            CurvePart part;
            part.volume     = v;
            part.mesh       = mo->volumes[v]->get_mesh_shared_ptr();
            part.trafo      = inst_trafo * mo->volumes[v]->get_matrix();
            part.has_issues = part.mesh && part.mesh->stats().has_any_issue();
            if (part.mesh && !part.mesh->its.indices.empty())
                parts.push_back(std::move(part));
        }
    return parts;
}

void curve_request_redraw()
{
    if (wxTheApp)
        wxTheApp->CallAfter([]() {
            if (Plater *plater = wxGetApp().plater())
                if (GLCanvas3D *canvas = plater->get_current_canvas3D()) {
                    canvas->set_as_dirty();
                    canvas->request_extra_frame();
                }
        });
}
} // namespace

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
        m_curve_preview->wanted  = gen; // any older run stops at its next check
        m_curve_preview->running = false;
        m_curve_preview->error.clear();
    }
    const bool has_geometry = m_shape_mode ? m_curve_shape.valid() : m_curve_anchors.size() >= 2;
    if (!m_curve_mode || !has_geometry || m_state != On) {
        m_curve_pieces.clear();
        m_curve_preview_failed = false;
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
    if (!curve_build_cutters(cutters)) {
        std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
        m_curve_preview->error = cutters.error;
        return;
    }
    std::vector<CurvePart> parts = curve_collect_parts(mo, instance_idx);
    if (parts.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
        m_curve_preview->running = true;
        m_curve_preview->started = std::chrono::steady_clock::now();
        m_curve_preview->stage   = _u8L("Starting...");
    }

    // The worker owns copies of everything it uses (the cutters, shared read-only meshes, the shared
    // state), so the gizmo can close or the object change while it runs.
    std::thread([state = m_curve_preview, gen, cutters = std::move(cutters), parts = std::move(parts)]() mutable {
        const std::function<bool()> cancelled = [&state, gen]() {
            std::lock_guard<std::mutex> lock(state->mutex);
            return state->wanted != gen;
        };
        const std::function<void(const std::string &)> stage = [&state, gen](const std::string &text) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->wanted == gen)
                state->stage = text;
        };
        CurveJobResult result;
        bool           finished = false;
        try {
            finished = run_curve_job(cutters, parts, state.get(), cancelled, stage, result);
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Curve cut preview failed: " << e.what();
            finished     = true;
            result.ok    = false;
            result.error = _u8L("The preview could not be calculated. Click \"Repair model\" or simplify the model and try again.");
        }
        if (!finished)
            return; // a newer outline took over
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->wanted != gen)
                return;
            state->pieces     = std::move(result.pieces);
            state->failed     = !result.ok;
            state->simplified = result.simplified;
            state->error      = std::move(result.error);
            state->skipped    = std::move(result.skipped);
            state->running    = false;
            state->ready      = gen;
        }
        curve_request_redraw();
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
        m_curve_preview_shown  = m_curve_generation;
        m_curve_preview_failed = m_curve_preview->failed;
        if (m_curve_preview->failed)
            return; // keep showing the last good preview; the panel says what went wrong
        pieces = m_curve_preview->pieces; // copy: the shared state stays valid for later reads
    }
    m_curve_pieces.clear();
    for (CurvePreview::Piece &p : pieces) {
        auto piece    = std::make_shared<CurvePiece>();
        piece->volume = p.volume;
        piece->in_a   = p.in_a;
        piece->orig_a = p.in_a;
        piece->model  = std::make_shared<GLModel>();
        piece->model->init_from(p.its);
        piece->mesh      = std::make_shared<TriangleMesh>(std::move(p.its));
        piece->raycaster = std::make_shared<MeshRaycaster>(*piece->mesh);
        m_curve_pieces.push_back(std::move(piece));
    }
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
    if (!m_curve_mode || m_connectors_editing || m_state != On)
        return;
    if (m_conn_editing) {
        // ghost connector where a click would place one
        double t = 0.0, z = 0.0;
        if (mouse_event.Moving() && !m_conn_dragging && curve_pick_wall(Vec2d(mouse_event.GetX(), mouse_event.GetY()), t, z))
            m_conn_hover = std::make_pair(t, z);
        else if (mouse_event.Moving() || mouse_event.Leaving())
            m_conn_hover.reset();
        if (mouse_event.Moving() || mouse_event.Leaving())
            m_parent.request_extra_frame();
        return;
    }
    if (m_shape_mode)
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
    // connectors as 3D shapes: selected green, separate pieces yellow, ones that could not be added red,
    // plus a see-through ghost where a click would place a new one
    const bool ghost = m_conn_editing && m_conn_hover && !m_conn_dragging;
    if (!m_curve_conns.empty() || ghost) {
        std::vector<int> skipped;
        if (m_curve_preview) {
            std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
            skipped = m_curve_preview->skipped;
        }
        const OutlineSampler sampler(curve_outline(false), m_shape_mode);
        if (sampler.valid()) {
            const BoundingBoxf3 tbb  = transformed_bounding_box(m_plane_center, m_rotate_matrix);
            const double        z_lo = tbb.defined ? tbb.min.z() : -m_radius, z_hi = tbb.defined ? tbb.max.z() : m_radius;
            auto draw = [&](double t, double z_in, const CurveConnParams &c, const ColorRGBA &color) {
                ConnFrame f;
                sampler.at(t, f.p, f.tangent);
                f.dir = Vec2d(f.tangent.y(), -f.tangent.x());
                if (c.on_part_b && !c.separate())
                    f.dir = -f.dir;
                indexed_triangle_set its = conn_display_solid(f, std::clamp(z_in, z_lo, z_hi), c, z_lo, z_hi);
                if (its.indices.empty())
                    return;
                its_transform(its, plane, true);
                GLModel model;
                model.init_from(its);
                render_glmodel(model, color.get_data(), camera.get_view_matrix(), camera.get_projection_matrix());
            };
            glsafe(::glEnable(GL_DEPTH_TEST));
            const double pick_tol = std::max(1.5, m_radius * 0.06);
            bool         near_one = false;
            for (size_t i = 0; i < m_curve_conns.size(); ++i) {
                const CurveConnector &conn = m_curve_conns[i];
                const bool failed = std::find(skipped.begin(), skipped.end(), int(i)) != skipped.end();
                const ColorRGBA color = int(i) == m_conn_selected ? ColorRGBA::GREEN() :
                                        failed                    ? ColorRGBA::RED() :
                                        conn.params.separate()    ? ColorRGBA::YELLOW() :
                                                                    ColorRGBA::ORANGE();
                draw(conn.t, conn.z, conn.params, color);
                if (ghost) {
                    Vec2d p, q, tg;
                    sampler.at(conn.t, p, tg);
                    sampler.at(m_conn_hover->first, q, tg);
                    near_one |= Vec3d(p.x() - q.x(), p.y() - q.y(), conn.z - m_conn_hover->second).norm() < pick_tol;
                }
            }
            if (ghost && !near_one) {
                // the settings a click would use
                ColorRGBA color = m_conn.separate() ? ColorRGBA::YELLOW() : ColorRGBA::ORANGE();
                color.a(0.45f);
                glsafe(::glEnable(GL_BLEND));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                glsafe(::glDepthMask(GL_FALSE));
                draw(m_conn_hover->first, m_conn_hover->second, m_conn, color);
                glsafe(::glDepthMask(GL_TRUE));
                glsafe(::glDisable(GL_BLEND));
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
    bool       show_repair  = false;
    if (has_geometry && m_curve_drag_idx < 0 && m_curve_bend_span < 0 && m_shape_drag == 0) {
        bool             failed = false, ready = false, running = false, simplified = false;
        std::string      stage, error;
        std::vector<int> skipped;
        double           seconds = 0.0;
        if (m_curve_preview) {
            std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
            ready      = m_curve_preview->ready == m_curve_generation;
            failed     = ready && m_curve_preview->failed;
            simplified = ready && m_curve_preview->simplified;
            running    = m_curve_preview->running;
            stage      = m_curve_preview->stage;
            error      = m_curve_preview->error;
            skipped    = ready ? m_curve_preview->skipped : std::vector<int>();
            seconds    = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_curve_preview->started).count();
        }
        if (running) {
            // what is being calculated and for how long, so a big model does not look stuck
            m_imgui->text(from_u8(stage) + wxString::Format(" (%d s)", int(seconds)));
            if (!m_curve_pieces.empty())
                m_imgui->text(_L("The last preview stays visible meanwhile."));
            m_parent.schedule_extra_frame(500);
        } else if (!error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + m_imgui->scaled(22.f));
            m_imgui->text(from_u8(error));
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            if (failed) {
                m_imgui->text(_L("\"Perform cut\" still tries the cut on the full model."));
                show_repair = true;
            }
        } else if (ready) {
            m_imgui->text(m_curve_pieces.empty() ? _L("The outline does not cross the model.") : _L("Preview: part A cyan, part B magenta."));
            if (simplified)
                m_imgui->text(_L("Preview on a simplified copy; the cut uses the full model."));
        }
        if (!skipped.empty()) {
            wxString list;
            for (int i : skipped)
                list += (list.empty() ? "" : ", ") + wxString::Format("%d", i + 1);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
            m_imgui->text(wxString::Format(_L("Connector %s could not be added here (shown red): move it or make it smaller."), list));
            ImGui::PopStyleColor();
        }
    }

    // model problems the cut may trip over, with an in-place repair
    {
        const Selection &selection  = m_parent.get_selection();
        const int        object_idx = selection.get_object_idx();
        const Model     *model      = selection.get_model();
        if (model != nullptr && object_idx >= 0 && object_idx < int(model->objects.size())) {
            const TriangleMeshStats stats = model->objects[object_idx]->get_object_stl_stats();
            if (stats.has_any_issue()) {
                wxString issues;
                if (stats.open_edges > 0)
                    issues += wxString::Format(_L("%d open edges"), stats.open_edges);
                if (stats.non_manifold_edges > 0)
                    issues += (issues.empty() ? "" : ", ") + wxString::Format(_L("%d non-manifold edges"), stats.non_manifold_edges);
                if (stats.has_reversed_faces)
                    issues += (issues.empty() ? "" : ", ") + _L("reversed faces");
                m_imgui->text(_L("Model problems") + ": " + issues);
                show_repair = true;
            }
        }
        if (!m_curve_repair_msg.empty())
            m_imgui->text(from_u8(m_curve_repair_msg));
        if (show_repair) {
            if (m_imgui->button(_L("Repair model")) && !m_curve_repair_requested) {
                // runs from the event loop: it shows a progress dialog, which cannot be done while rendering
                m_curve_repair_requested = true;
                wxGetApp().plater()->CallAfter([this]() {
                    m_curve_repair_requested = false;
                    if (m_state == On && m_curve_mode)
                        curve_repair_model();
                });
            }
            if (ImGui::IsItemHovered())
                m_imgui->tooltip(is_win10_model_repair_available() ?
                                     _L("Merges loose vertices, removes broken and duplicate faces and turns reversed faces around. Open and non-manifold "
                                        "edges are closed by the Windows repair service (like \"Fix model\"). Undo with Ctrl+Z.") :
                                     _L("Merges loose vertices, removes broken and duplicate faces and turns reversed faces around. Undo with Ctrl+Z."),
                                 m_imgui->scaled(20.f));
        }
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

    // every connector, with whether it is part of A, part of B or a separate piece; click to select
    if (!m_curve_conns.empty()) {
        const char *type_names[] = {"Dovetail", "Pin", "Snap-fit", "Dowel", "Bowtie key"};
        for (int i = 0; i < int(m_curve_conns.size()); ++i) {
            const CurveConnParams &p     = m_curve_conns[i].params;
            const int              t     = std::clamp(int(p.type), 0, 4);
            const std::string      owner = p.separate() ? _u8L("separate piece") : p.on_part_b ? _u8L("on part B") : _u8L("on part A");
            const std::string      label = std::to_string(i + 1) + ". " + _u8L(type_names[t]) + " - " + owner + "##curve_conn" + std::to_string(i);
            ImGui::PushStyleColor(ImGuiCol_Text, p.separate() ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) : ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
            if (ImGui::Selectable(label.c_str(), i == m_conn_selected)) {
                m_conn_selected = i;
                m_conn          = p;
            }
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }

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
        // the exact value can be typed in too
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(3.5f));
        float typed = float(value);
        if (ImGui::BBLDragFloat(("##conn_in_" + label.ToStdString()).c_str(), &typed, 0.05f, 0.0f, 0.0f, fmt)) {
            typed = std::clamp(typed, float(min_value), float(max_value));
            if (std::abs(typed - value) > EPSILON) {
                value   = double(typed);
                changed = true;
            }
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
    if (c.type == ConnType::Dovetail || c.type == ConnType::Pin || c.type == ConnType::Snap) {
        // a loose piece printed on its own, with a matching hole in both parts
        changed |= m_imgui->bbl_checkbox(_L("Separate piece (holes in both parts)"), c.loose);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Dovetail: a double dovetail key. Pin: a loose pin. Snap-fit: a clip with a barb on each side. Shown in yellow."),
                             m_imgui->scaled(20.f));
    }
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
    if (!(m_keep_upper || m_keep_lower) || m_curve_drag_idx >= 0 || m_curve_bend_span >= 0 || m_shape_drag != 0 || m_conn_dragging)
        return false;
    const bool has_geometry = m_shape_mode ? m_curve_shape.valid() : m_curve_anchors.size() >= 2;
    if (!has_geometry)
        return false;
    // A finished preview tells whether the outline splits the model. While it is still running, or when
    // it failed, the cut is calculated on the full model and reports its own problems.
    if (curve_preview_ready()) {
        bool any_a = false, any_b = false;
        for (const auto &piece : m_curve_pieces)
            (piece->in_a ? any_a : any_b) = true;
        return any_a && any_b;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cut

namespace {
// State shared between a modal job on a worker thread and the progress dialog waiting for it.
struct CurveModalJob
{
    std::atomic<bool> done{false};
    std::atomic<bool> cancel{false};
    std::mutex        mutex;
    std::string       stage;
};

// Shows a progress dialog until the job is done. Returns false when the user cancelled.
bool curve_wait_modal(const std::shared_ptr<CurveModalJob> &job, const wxString &title)
{
    ProgressDialog dlg(title, _L("Starting..."), 100, wxGetApp().mainframe, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
    const auto     started = std::chrono::steady_clock::now();
    while (!job->done) {
        wxString msg;
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            msg = from_u8(job->stage);
        }
        msg += wxString::Format(" (%d s)", int(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()));
        if (!dlg.Pulse(msg)) {
            job->cancel = true; // the worker stops at its next check and cleans up on its own
            return false;
        }
        wxMilliSleep(100);
    }
    dlg.Update(100);
    return true;
}

double bbox_overlap(const BoundingBoxf3 &a, const BoundingBoxf3 &b)
{
    if (!a.defined || !b.defined)
        return 0.0;
    const Vec3d lo = a.min.cwiseMax(b.min), hi = a.max.cwiseMin(b.max);
    if ((hi - lo).minCoeff() <= 0.0)
        return 0.0;
    const double inter = (hi - lo).prod();
    const double va = a.size().prod(), vb = b.size().prod();
    return inter / std::max(va + vb - inter, EPSILON);
}
} // namespace

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

    CurveCutters cutters;
    if (!curve_build_cutters(cutters)) {
        MessageDialog(wxGetApp().mainframe, cutters.error.empty() ? _L("The outline cannot be used for a cut.") : from_u8(cutters.error),
                      _L("Cut"), wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }

    // The pieces to cut with: the preview when it was made on the full model, else a full-resolution run.
    // Either way the pieces keep the side the user gave them in the preview.
    std::vector<CurvePreview::Piece> pieces;
    std::vector<int>                 skipped;
    bool                             use_preview = false;
    if (m_curve_preview && curve_preview_ready()) {
        std::lock_guard<std::mutex> lock(m_curve_preview->mutex);
        use_preview = !m_curve_preview->failed && !m_curve_preview->simplified;
        skipped     = m_curve_preview->skipped;
    }
    if (use_preview) {
        for (const auto &piece : m_curve_pieces)
            pieces.push_back({piece->volume, piece->in_a, piece->mesh->its});
    } else {
        std::vector<CurvePart> parts = curve_collect_parts(mo, instance_idx);
        auto                   job   = std::make_shared<CurveModalJob>();
        auto                   res   = std::make_shared<CurveJobResult>();
        auto                   own   = std::make_shared<CurveCutters>(cutters); // the keys below still need 'cutters'
        std::thread([job, res, own, parts = std::move(parts)]() {
            try {
                run_curve_job(
                    *own, parts, nullptr, [&job]() { return job->cancel.load(); },
                    [&job](const std::string &text) {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->stage = text;
                    },
                    *res);
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(error) << "Curve cut failed: " << e.what();
                res->ok = false;
            }
            job->done = true;
        }).detach();
        if (!curve_wait_modal(job, _L("Cutting")) || job->cancel)
            return;
        // the dialog pumped events: make sure nothing changed under us
        if (m_state != On || object_idx >= int(plater->model().objects.size()) || plater->model().objects[object_idx] != mo)
            return;

        bool any_a = false, any_b = false;
        for (const CurvePreview::Piece &p : res->pieces)
            (p.in_a ? any_a : any_b) = true;
        if (!res->ok || !any_a || !any_b) {
            const wxString reason = !res->ok ? (res->error.empty() ? _L("The cut could not be calculated.") : from_u8(res->error)) :
                                               _L("The outline does not split the model into two parts.");
            if (curve_preview_visible() && !m_curve_preview_failed) {
                // the (simplified) preview worked: offer it rather than nothing
                MessageDialog dlg(wxGetApp().mainframe,
                                  reason + "\n\n" + _L("Cut using the preview instead? It was calculated on a simplified copy, so fine detail is lost."),
                                  _L("Cut"), wxYES_NO | wxICON_WARNING);
                if (dlg.ShowModal() != wxID_YES)
                    return;
                for (const auto &piece : m_curve_pieces)
                    pieces.push_back({piece->volume, piece->in_a, piece->mesh->its});
            } else {
                MessageDialog(wxGetApp().mainframe, reason, _L("Cut"), wxOK | wxICON_WARNING).ShowModal();
                return;
            }
        } else {
            pieces  = std::move(res->pieces);
            skipped = res->skipped;
            // keep the sides chosen on the preview: match every piece to the preview piece it overlaps most
            std::vector<BoundingBoxf3> preview_boxes;
            for (const auto &piece : m_curve_pieces)
                preview_boxes.push_back(piece->mesh->bounding_box());
            for (CurvePreview::Piece &p : pieces) {
                BoundingBoxf3 box;
                for (const Vec3f &v : p.its.vertices)
                    box.merge(v.cast<double>());
                const bool computed_a = p.in_a;
                double     best       = 0.3; // too little overlap: keep the computed side
                for (size_t i = 0; i < m_curve_pieces.size(); ++i)
                    if (m_curve_pieces[i]->volume == p.volume && m_curve_pieces[i]->orig_a == computed_a) {
                        const double o = bbox_overlap(box, preview_boxes[i]);
                        if (o > best) {
                            best   = o;
                            p.in_a = m_curve_pieces[i]->in_a;
                        }
                    }
            }
        }
    }

    // Results per side and volume (world -> volume coordinates)
    const Transform3d inst_trafo = mo->instances[instance_idx]->get_matrix();
    auto collect = [&](bool side_a) {
        std::vector<std::optional<indexed_triangle_set>> meshes(mo->volumes.size());
        for (size_t v = 0; v < mo->volumes.size(); ++v)
            if (mo->volumes[v]->is_model_part())
                meshes[v] = indexed_triangle_set();
        for (const CurvePreview::Piece &piece : pieces)
            if (piece.in_a == side_a && piece.volume < meshes.size() && meshes[piece.volume])
                its_merge(*meshes[piece.volume], piece.its);
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

    // Separate connector pieces (dowels, keys, loose pins and clips) become objects of their own, printed apart
    // and fitted into the holes of both parts afterwards: turned to print well, set down on the plate and
    // lined up in front of the cut object, the way the planar cut places its dowels. Ones whose holes could
    // not be made are left out.
    int keys_left_out = 0;
    {
        const BoundingBoxf3 obj_box = mo->instance_bounding_box(size_t(instance_idx));
        const double        gap     = 5.0;
        double              x       = obj_box.min.x();
        for (CurveCutters::Key &k : cutters.keys) {
            if (std::find(skipped.begin(), skipped.end(), k.conn) != skipped.end()) {
                ++keys_left_out;
                continue;
            }
            TriangleMesh key = std::move(k.mesh);
            if (k.up.squaredNorm() > EPSILON)
                key.transform(Transform3d(Eigen::Quaterniond::FromTwoVectors(k.up, Vec3d::UnitZ())));
            const BoundingBoxf3 box = key.bounding_box();
            key.translate(-box.center().cast<float>());
            const Vec3d size = box.size();
            ModelObject *obj = scratch.add_object();
            obj->name        = mo->name + " - " + _u8L("connector") + " " + std::to_string(k.conn + 1);
            obj->add_volume(std::move(key));
            obj->add_instance()->set_offset(Vec3d(x + 0.5 * size.x(), obj_box.min.y() - gap - 0.5 * size.y(), 0.5 * size.z()));
            obj->invalidate_bounding_box();
            results.push_back(obj);
            x += size.x() + gap;
        }
    }

    m_parent.reset_all_gizmos();
    Plater::TakeSnapshot snapshot(plater, m_shape_mode ? "Cut by shape" : "Cut by curve");
    plater->apply_cut_object_to_model(size_t(object_idx), results);
    curve_reset();
    if (keys_left_out > 0)
        MessageDialog(wxGetApp().mainframe,
                      wxString::Format(_L("%d separate connector piece(s) were left out: their holes could not be made in the parts. "
                                          "Try a different size or position for them."),
                                       keys_left_out),
                      _L("Cut"), wxOK | wxICON_WARNING)
            .ShowModal();
}

// ---------------------------------------------------------------------------------------------------------------------
// Repair

void GLGizmoAdvancedCut::curve_repair_model()
{
    const Selection &selection  = m_parent.get_selection();
    const int        object_idx = selection.get_object_idx();
    Plater          *plater     = wxGetApp().plater();
    if (object_idx < 0 || object_idx >= int(plater->model().objects.size()))
        return;
    ModelObject *mo = plater->model().objects[object_idx];

    // repaired copies are made on a worker thread; the meshes are shared read-only
    struct Repaired
    {
        size_t                              volume{0};
        std::shared_ptr<const TriangleMesh> src;
        indexed_triangle_set                its;
        RepairCounts                        counts;
        bool                                win10{false}; // also went through the Windows repair service
    };
    std::vector<std::pair<size_t, std::shared_ptr<const TriangleMesh>>> sources;
    for (size_t v = 0; v < mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part())
            sources.emplace_back(v, mo->volumes[v]->get_mesh_shared_ptr());
    // Open and non-manifold edges need the Windows repair service (the same one as "Fix model"); the quick
    // repair below only turns faces and removes broken ones.
    const TriangleMeshStats before    = mo->get_object_stl_stats();
    const bool              use_win10 = (before.open_edges > 0 || !before.manifold()) && is_win10_model_repair_available();
    auto job = std::make_shared<CurveModalJob>();
    auto out = std::make_shared<std::vector<Repaired>>();
    std::thread([job, out, sources, use_win10]() {
        try {
            for (size_t i = 0; i < sources.size() && !job->cancel; ++i) {
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->stage = sources.size() > 1 ? (boost::format(_u8L("Repairing part %1% of %2%...")) % (i + 1) % sources.size()).str() :
                                                      _u8L("Repairing the model...");
                }
                Repaired r;
                r.volume = sources[i].first;
                r.src    = sources[i].second;
                r.its    = sources[i].second->its;
                r.counts = repair_its(r.its);
                if (use_win10 && !job->cancel) {
                    const std::string part = sources.size() > 1 ? (boost::format(_u8L("Part %1% of %2%: ")) % (i + 1) % sources.size()).str() : std::string();
                    indexed_triangle_set fixed;
                    std::string          error;
                    const bool ok = fix_mesh_by_win10_sdk(
                        r.its, fixed,
                        [job, part](const char *message, unsigned progress) {
                            std::lock_guard<std::mutex> lock(job->mutex);
                            job->stage = part + _u8L("Windows repair service") + ": " + (message ? message : "") + " (" + std::to_string(progress) + "%)";
                        },
                        [job]() { return job->cancel.load(); }, &error);
                    if (ok && !fixed.indices.empty()) {
                        r.its   = std::move(fixed);
                        r.win10 = true;
                    } else
                        BOOST_LOG_TRIVIAL(warning) << "Curve cut: Windows repair service failed: " << error;
                }
                out->push_back(std::move(r));
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "Curve cut: repair failed: " << e.what();
            out->clear();
        }
        job->done = true;
    }).detach();
    if (!curve_wait_modal(job, _L("Repair model")) || job->cancel)
        return;
    if (m_state != On || object_idx >= int(plater->model().objects.size()) || plater->model().objects[object_idx] != mo || out->empty())
        return;

    RepairCounts total;
    bool         win10 = false;
    {
        Plater::TakeSnapshot snapshot(plater, "Repair model");
        for (Repaired &r : *out) {
            if (r.volume >= mo->volumes.size() || mo->volumes[r.volume]->get_mesh_shared_ptr() != r.src || r.its.indices.empty())
                continue; // changed meanwhile
            total.removed += r.counts.removed;
            total.flipped += r.counts.flipped;
            win10 |= r.win10;
            ModelVolume *vol = mo->volumes[r.volume];
            vol->set_mesh_keep_paint(TriangleMesh(std::move(r.its))); // painting is carried over approximately
            vol->calculate_convex_hull();
        }
        mo->invalidate_bounding_box();
    }
    plater->changed_mesh(object_idx);
    if (ObjectList *list = wxGetApp().obj_list()) {
        list->update_item_error_icon(object_idx, -1);
        list->update_info_items(size_t(object_idx));
    }
    update_bb();

    const TriangleMeshStats stats = mo->get_object_stl_stats();
    std::string             msg   = (boost::format(_u8L("Repaired: %1% faces removed, %2% faces turned.")) % total.removed % total.flipped).str();
    if (win10)
        msg += " " + _u8L("Open and non-manifold edges were fixed by the Windows repair service.");
    else if (use_win10)
        msg += " " + _u8L("The Windows repair service failed; try \"Fix model\" from the object's right-click menu after closing the Cut tool.");
    if (stats.has_any_issue())
        msg += " " + (boost::format(_u8L("Still %1% open and %2% non-manifold edges: the cut may still fail there.")) % stats.open_edges %
                      stats.non_manifold_edges)
                         .str();
    else
        msg += " " + _u8L("No problems left.");
    m_curve_repair_msg = msg;
    curve_changed();
    m_parent.set_as_dirty();
}

} // namespace GUI
} // namespace Slic3r
