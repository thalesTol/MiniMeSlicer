// SliceOfMe: texture tools for the Color painting gizmo.
//
//  - Pattern fill:  click a colour area; it is filled with stripes, checkers, dots or a dithered gradient made of
//                   the selected filament and a second one. The pattern is laid out in the current view.
//  - Image decal:   an image (PNG/JPG) is projected from the current view onto the faces looking at the camera;
//                   every pixel is matched to the nearest filament colour, transparent pixels are left alone.
//  - Texture brush: the same projection, painted with the brush where you stroke (the image repeats).
//  - Image relief:  the image's dark / light areas, or any painted colour, turned into an engraved or raised
//                   shell that follows the surface (a negative part or a part of the same filament).
//
// Triangles are subdivided where the pattern or image needs detail (TriangleSelector::paint_region /
// set_facets_with_subdivision), so the result follows the picture instead of the original triangles.

#include "GLGizmoMmuSegmentation.hpp"

#include <cmath>
#include <map>
#include <set>
#include <memory>

#include <wx/filedlg.h>
#include <wx/image.h>
#include <wx/utils.h>
#include <boost/log/trivial.hpp>

#include "GLGizmosCommon.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"
#include "libslic3r/SLA/IndexedMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <algorithm>

#include <imgui/imgui.h>

namespace Slic3r {
namespace GUI {

namespace {

constexpr size_t TEX_NODE_BUDGET = 1500000;
constexpr int    TEX_MAX_DEPTH   = 14;
constexpr int    TEX_MAX_IMAGE   = 512; // longest image side after loading, pixels

// 2D coordinates in the texture frame (u right, v up), rotated by 'angle_deg'
Vec2d tex_uv(const Vec3d &p, const Vec3d &origin, const Vec3d &right, const Vec3d &up, double angle_deg)
{
    const Vec3d  d = p - origin;
    const double u = d.dot(right), v = d.dot(up);
    const double a = angle_deg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
    return Vec2d(c * u + s * v, -s * u + c * v);
}

// sample points of a triangle: corners, edge midpoints, centroid
std::array<Vec3f, 7> tri_samples(const std::array<Vec3f, 3> &t)
{
    return {t[0], t[1], t[2], 0.5f * (t[0] + t[1]), 0.5f * (t[1] + t[2]), 0.5f * (t[2] + t[0]), (t[0] + t[1] + t[2]) / 3.f};
}

double tri_area(const std::array<Vec3d, 3> &t) { return 0.5 * (t[1] - t[0]).cross(t[2] - t[0]).norm(); }

} // namespace

bool GLGizmoMmuSegmentation::gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    if (m_tex_tool != TextureTool::None && texture_gizmo_event(action, mouse_position, shift_down, alt_down, control_down))
        return true;
    return GLGizmoPainterBase::gizmo_event(action, mouse_position, shift_down, alt_down, control_down);
}

bool GLGizmoMmuSegmentation::texture_hit(const Vec2d &mouse_position, int &mesh_idx, int &facet, Vec3f &hit_mesh, Vec3d &hit_world) const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_c->raycaster() == nullptr)
        return false;
    const Selection &selection = m_parent.get_selection();
    const Camera    &camera    = wxGetApp().plater()->get_camera();
    const auto      &casters   = m_c->raycaster()->raycasters();
    double           best      = std::numeric_limits<double>::max();
    int              part      = -1;
    mesh_idx                   = -1;
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_model_part())
            continue;
        ++part;
        if (part >= int(casters.size()))
            break;
        const Transform3d trafo = get_volume_world_matrix(selection, mo, mv, volume_id);
        Vec3f             pos, normal;
        size_t            f = 0;
        if (casters[part]->unproject_on_mesh(mouse_position, trafo, camera, pos, normal, nullptr, &f)) {
            const Vec3d  world = trafo * pos.cast<double>();
            const double d     = (world - camera.get_position()).squaredNorm();
            if (d < best) {
                best      = d;
                mesh_idx  = part;
                facet     = int(f);
                hit_mesh  = pos;
                hit_world = world;
            }
        }
    }
    return mesh_idx >= 0;
}

bool GLGizmoMmuSegmentation::texture_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    // relief from a painted colour: the normal painting tools stay active
    if (m_tex_tool == TextureTool::Relief && m_relief_source == 1)
        return false;
    // Shift + wheel rotates, Ctrl / Alt + wheel resizes (the pattern, the decal or the relief image).
    // Alt + wheel would otherwise move the clipping plane, which is slow on big models.
    const bool wheel = action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown;
    if (wheel && m_tex_tool != TextureTool::Brush && (shift_down || alt_down || control_down)) {
        const bool up = action == SLAGizmoEventType::MouseWheelUp;
        if (m_tex_tool == TextureTool::Pattern) {
            if (shift_down) {
                m_pat_angle += up ? 5.f : -5.f;
                if (m_pat_angle > 90.f) m_pat_angle -= 180.f;
                if (m_pat_angle < -90.f) m_pat_angle += 180.f;
            } else
                m_pat_scale = std::clamp(m_pat_scale * (up ? 1.1f : 1.f / 1.1f), 0.5f, 30.f);
        } else if (shift_down) {
            m_decal_angle += up ? 5.f : -5.f;
            if (m_decal_angle > 180.f) m_decal_angle -= 360.f;
            if (m_decal_angle < -180.f) m_decal_angle += 360.f;
        } else
            m_decal_width = std::clamp(m_decal_width * (up ? 1.1f : 1.f / 1.1f), 2.f, 300.f);
        m_parent.set_as_dirty();
        return true;
    }
    if (control_down)
        return false; // Ctrl + drag rotates the view
    int   mesh_idx = -1, facet = -1;
    Vec3f hit_mesh;
    Vec3d hit_world;
    const Camera &camera = wxGetApp().plater()->get_camera();
    auto take_frame = [&](const Vec3d &origin) {
        m_tex_origin  = origin;
        m_tex_right   = camera.get_dir_right();
        m_tex_up      = camera.get_dir_up();
        m_tex_forward = camera.get_dir_forward();
    };

    switch (m_tex_tool) {
    case TextureTool::Pattern:
        if (action == SLAGizmoEventType::Moving) {
            // show the pattern on the face under the mouse, on a light copy of the face: re-colouring the whole
            // painted part on every move was slow on big models
            if (texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world) && mesh_idx < int(m_triangle_selectors.size())) {
                const bool inside = m_pat_hover_mesh == mesh_idx && m_pat_hover_angle == m_pat_face_angle && facet >= 0 &&
                                    facet < int(m_pat_hover_region.size()) && m_pat_hover_region[facet];
                if (!inside) {
                    texture_clear_pattern_hover();
                    int                 volume_id = -1;
                    std::vector<int>    region    = texture_face_region(mesh_idx, facet, &volume_id);
                    const ModelObject  *mo        = m_c->selection_info()->model_object();
                    if (!region.empty() && mo != nullptr) {
                        std::sort(region.begin(), region.end());
                        m_pat_hover_region.assign(mo->volumes[volume_id]->mesh().its.indices.size(), 0);
                        for (int f : region)
                            m_pat_hover_region[f] = 1;
                        m_pat_hover_facets = std::move(region);
                        m_pat_hover_volume = volume_id;
                        m_pat_hover_trafo  = get_volume_world_matrix(m_parent.get_selection(), mo, mo->volumes[volume_id], volume_id);
                        m_pat_hover_mesh   = mesh_idx;
                        m_pat_hover_angle  = m_pat_face_angle;
                        m_pat_hover_dirty  = true;
                    }
                } else if (!camera.get_dir_right().isApprox(m_pat_hover_cam_right, 1e-4) || !camera.get_dir_up().isApprox(m_pat_hover_cam_up, 1e-4))
                    m_pat_hover_dirty = true; // the view turned: the pattern follows it
            } else
                texture_clear_pattern_hover();
            m_parent.set_as_dirty();
            return true;
        }
        if (action == SLAGizmoEventType::LeftDown) {
            if (!texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world))
                return false; // off the model: rotate the view
            texture_apply_pattern(mesh_idx, facet);
            texture_clear_pattern_hover();
            texture_commit("Pattern fill");
            return true;
        }
        return action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Dragging;
    case TextureTool::Decal:
    case TextureTool::Relief:
        if (action == SLAGizmoEventType::Moving) {
            // preview the image where it would land
            if (m_img_w > 0 && texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world)) {
                m_decal_hover         = hit_world;
                m_decal_hover_right   = camera.get_dir_right();
                m_decal_hover_up      = camera.get_dir_up();
                m_decal_hover_forward = camera.get_dir_forward();
            } else
                m_decal_hover.reset();
            m_parent.set_as_dirty();
            return true;
        }
        if (action == SLAGizmoEventType::LeftDown) {
            if (m_img_w == 0 || !texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world))
                return false;
            take_frame(hit_world);
            if (m_tex_tool == TextureTool::Relief) {
                texture_apply_relief(hit_world);
                return true;
            }
            texture_apply_image(hit_world, false);
            texture_commit("Image decal");
            return true;
        }
        return action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Dragging;
    case TextureTool::Brush:
        if (action == SLAGizmoEventType::LeftDown || (action == SLAGizmoEventType::Dragging && m_brush_stroke)) {
            if (m_img_w == 0 || !texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world))
                return action == SLAGizmoEventType::Dragging;
            if (action == SLAGizmoEventType::LeftDown) {
                take_frame(hit_world); // the image stays put for the whole stroke
                m_brush_stroke = true;
            } else if ((mouse_position - m_brush_last_mouse).norm() < 6.0 || (hit_world - m_brush_last_hit).norm() < 0.25 * m_cursor_radius)
                return true; // stamp only after the mouse moved a bit (a quarter of the brush): keeps the brush light
            m_brush_last_mouse = mouse_position;
            m_brush_last_hit   = hit_world;
            texture_apply_image(hit_world, true, mesh_idx, facet);
            return true;
        }
        if (action == SLAGizmoEventType::LeftUp && m_brush_stroke) {
            m_brush_stroke = false;
            texture_commit("Texture brush");
            return true;
        }
        return false;
    default: return false;
    }
}

void GLGizmoMmuSegmentation::texture_update_states()
{
    // match every pixel to the nearest filament colour once, not on every stroke
    const size_t n = size_t(m_img_w) * m_img_h;
    if (m_img_states.size() == n && m_img_states_colors == m_extruders_colors)
        return;
    m_img_states.assign(n, -1);
    m_img_states_colors = m_extruders_colors;
    for (size_t i = 0; i < n; ++i) {
        if (!m_img_alpha.empty() && m_img_alpha[i] < 128)
            continue;
        const std::array<size_t, 3> rgb = {m_img_rgb[3 * i], m_img_rgb[3 * i + 1], m_img_rgb[3 * i + 2]};
        double best = std::numeric_limits<double>::max();
        for (size_t f = 0; f < m_extruders_colors.size() && f < size_t(EnforcerBlockerType::ExtruderMax); ++f) {
            const double d = compute_delta_e(rgb, m_extruders_colors[f]);
            if (d < best) {
                best            = d;
                m_img_states[i] = int(f);
            }
        }
    }
}

void GLGizmoMmuSegmentation::texture_clear_pattern_hover()
{
    m_pat_hover_mesh   = -1;
    m_pat_hover_volume = -1;
    m_pat_hover_region.clear();
    m_pat_hover_facets.clear();
    for (GLModel &model : m_pat_hover_models)
        model.reset();
    m_pat_hover_dirty = false;
}

std::pair<int, int> GLGizmoMmuSegmentation::texture_pattern_filaments() const
{
    const int n_filaments = std::max(1, int(m_extruders_colors.size()));
    const int primary     = std::clamp(int(m_selected_extruder_idx), 0, n_filaments - 1);
    int       second      = std::clamp(m_pat_second, 0, n_filaments - 1);
    // the same filament twice would fill the face with one colour: use the next one instead
    if (second == primary)
        second = (second + 1) % n_filaments;
    return {primary, second};
}

GLGizmoMmuSegmentation::PatternFrame GLGizmoMmuSegmentation::texture_pattern_frame(int volume_id, const std::vector<int> &region, const Transform3d &trafo) const
{
    PatternFrame       frame;
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || volume_id < 0 || volume_id >= int(mo->volumes.size()) || region.empty())
        return frame;
    const indexed_triangle_set &its = mo->volumes[volume_id]->mesh().its;
    auto corner = [&](int f, int k) -> Vec3d { return trafo * its.vertices[its.indices[f][k]].cast<double>(); };
    // the face's average direction; anchored at a fixed corner of the face, so the pattern stays put while the mouse moves
    Vec3d normal = Vec3d::Zero();
    for (int f : region)
        normal += (corner(f, 1) - corner(f, 0)).cross(corner(f, 2) - corner(f, 0));
    const int anchor = *std::min_element(region.begin(), region.end());
    if (normal.norm() < 1e-12)
        normal = (corner(anchor, 1) - corner(anchor, 0)).cross(corner(anchor, 2) - corner(anchor, 0));
    if (normal.norm() < 1e-12)
        normal = Vec3d::UnitZ();
    normal.normalize();
    // lay the pattern flat on the face: the view's right / up projected onto it
    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3d right = camera.get_dir_right() - camera.get_dir_right().dot(normal) * normal;
    if (right.norm() < 1e-3) // looking along the face: use the view's up instead
        right = camera.get_dir_up() - camera.get_dir_up().dot(normal) * normal;
    frame.right  = right.normalized();
    frame.up     = normal.cross(frame.right).normalized();
    frame.origin = corner(anchor, 0);
    // gradient: the extent of the face across the pattern direction
    if (m_pat_type == 3) {
        frame.u_min = std::numeric_limits<double>::max();
        frame.u_max = -frame.u_min;
        for (int f : region)
            for (int k = 0; k < 3; ++k) {
                const double u = tex_uv(corner(f, k), frame.origin, frame.right, frame.up, m_pat_angle).x();
                frame.u_min    = std::min(frame.u_min, u);
                frame.u_max    = std::max(frame.u_max, u);
            }
    }
    return frame;
}

bool GLGizmoMmuSegmentation::texture_pattern_second(const PatternFrame &frame, const Vec3d &world) const
{
    const double scale = std::max(0.2, double(m_pat_scale));
    const Vec2d  uv    = tex_uv(world, frame.origin, frame.right, frame.up, m_pat_angle);
    switch (m_pat_type) {
    case 0: return (int(std::floor(uv.x() / scale)) & 1) != 0; // stripes
    case 1: return ((int(std::floor(uv.x() / scale)) + int(std::floor(uv.y() / scale))) & 1) != 0; // checker
    case 2: { // dots
        const Vec2d cell((std::floor(uv.x() / scale) + 0.5) * scale, (std::floor(uv.y() / scale) + 0.5) * scale);
        return (uv - cell).norm() < 0.35 * scale;
    }
    default: { // gradient: fine stripes whose width grows across the face
        const double t    = frame.u_max > frame.u_min ? std::clamp((uv.x() - frame.u_min) / (frame.u_max - frame.u_min), 0.0, 1.0) : 0.5;
        const double frac = uv.y() / scale - std::floor(uv.y() / scale);
        return frac < t;
    }
    }
}

void GLGizmoMmuSegmentation::texture_build_pattern_hover() const
{
    for (GLModel &model : m_pat_hover_models)
        model.reset();
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_pat_hover_volume < 0 || m_pat_hover_volume >= int(mo->volumes.size()) || m_pat_hover_facets.empty() ||
        m_pat_hover_mesh < 0 || m_pat_hover_mesh >= int(m_triangle_selectors.size()))
        return;
    const indexed_triangle_set &its     = mo->volumes[m_pat_hover_volume]->mesh().its;
    const auto                 &normals = m_triangle_selectors[m_pat_hover_mesh]->get_face_normals();
    const Transform3d          &trafo   = m_pat_hover_trafo;
    const PatternFrame          frame   = texture_pattern_frame(m_pat_hover_volume, m_pat_hover_facets, trafo);
    const double                scale   = std::max(0.2, double(m_pat_scale));

    // Split the face's triangles until each one shows a single colour, like the click will (with fewer levels).
    // Huge faces of tiny triangles are not split at all: each triangle simply takes its colour.
    constexpr size_t MAX_TRIANGLES = 800000;
    constexpr int    MAX_DEPTH     = 7;
    std::array<GLModel::Geometry, 2> geo;
    for (auto &g : geo) {
        g.format = {GLModel::PrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
        g.reserve_vertices(3 * m_pat_hover_facets.size());
        g.reserve_indices(3 * m_pat_hover_facets.size());
    }
    std::array<unsigned int, 2> count{0, 0};
    size_t                      leaves = 0;
    struct Item { std::array<Vec3f, 3> v; int depth; };
    std::vector<Item> stack;
    for (size_t i = 0; i < m_pat_hover_facets.size(); ++i) {
        const int    f         = m_pat_hover_facets[i];
        const Vec3f &n         = normals[f];
        const bool   may_split = leaves + (m_pat_hover_facets.size() - i) < MAX_TRIANGLES; // leave room for the rest of the face
        stack.push_back({{its.vertices[its.indices[f][0]], its.vertices[its.indices[f][1]], its.vertices[its.indices[f][2]]}, 0});
        while (!stack.empty()) {
            const Item it = stack.back();
            stack.pop_back();
            int n_second = 0, n_samples = 0;
            for (const Vec3f &p : tri_samples(it.v)) {
                n_second += texture_pattern_second(frame, trafo * p.cast<double>()) ? 1 : 0;
                ++n_samples;
            }
            const Vec3d  w0 = trafo * it.v[0].cast<double>(), w1 = trafo * it.v[1].cast<double>(), w2 = trafo * it.v[2].cast<double>();
            const double longest = std::max({(w1 - w0).norm(), (w2 - w1).norm(), (w0 - w2).norm()});
            const bool   mixed   = n_second > 0 && n_second < n_samples;
            if (may_split && it.depth < MAX_DEPTH && leaves + stack.size() + 4 < MAX_TRIANGLES &&
                (longest > 0.5 * scale || (mixed && longest > scale / 8.0))) {
                const Vec3f m01 = 0.5f * (it.v[0] + it.v[1]), m12 = 0.5f * (it.v[1] + it.v[2]), m20 = 0.5f * (it.v[2] + it.v[0]);
                stack.push_back({{it.v[0], m01, m20}, it.depth + 1});
                stack.push_back({{m01, it.v[1], m12}, it.depth + 1});
                stack.push_back({{m20, m12, it.v[2]}, it.depth + 1});
                stack.push_back({{m01, m12, m20}, it.depth + 1});
                continue;
            }
            const int st = 2 * n_second > n_samples ? 1 : 0;
            for (const Vec3f &p : it.v)
                geo[st].add_vertex(p, n);
            geo[st].add_triangle(count[st], count[st] + 1, count[st] + 2);
            count[st] += 3;
            ++leaves;
        }
    }
    for (int st = 0; st < 2; ++st)
        if (!geo[st].is_empty())
            m_pat_hover_models[st].init_from(std::move(geo[st]));
}

std::vector<int> GLGizmoMmuSegmentation::texture_face_region(int mesh_idx, int facet, int *volume_id) const
{
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || mesh_idx < 0 || mesh_idx >= int(m_triangle_selectors.size()))
        return {};
    int vol = -1;
    for (int v = 0, part = -1; v < int(mo->volumes.size()); ++v)
        if (mo->volumes[v]->is_model_part() && ++part == mesh_idx) {
            vol = v;
            break;
        }
    if (vol < 0)
        return {};
    if (volume_id != nullptr)
        *volume_id = vol;
    // the same walk as the Fill tool's seed fill, over the part's own facets only
    TriangleSelectorGUI      &selector  = *m_triangle_selectors[mesh_idx];
    const std::vector<Vec3i> &neighbors = selector.get_neighbors();
    const std::vector<Vec3f> &normals   = selector.get_face_normals();
    if (facet < 0 || facet >= int(neighbors.size()) || neighbors.size() != normals.size())
        return {};
    const float       limit = float(std::cos(Geometry::deg2rad(double(m_pat_face_angle))) - EPSILON);
    std::vector<char> seen(neighbors.size(), 0);
    std::vector<int>  region{facet}, stack{facet};
    seen[facet] = 1;
    while (!stack.empty()) {
        const int f = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k) {
            const int nb = neighbors[f][k];
            if (nb >= 0 && nb < int(seen.size()) && !seen[nb] && std::clamp(normals[nb].dot(normals[f]), 0.f, 1.f) >= limit) {
                seen[nb] = 1;
                region.push_back(nb);
                stack.push_back(nb);
            }
        }
    }
    return region;
}

void GLGizmoMmuSegmentation::render_relief_volumes() const
{
    // the colour painting only draws model parts; show the engraved reliefs (negative parts) too
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr)
        return;
    const Camera &camera = wxGetApp().plater()->get_camera();
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_negative_volume())
            continue;
        std::shared_ptr<GLModel> &model = m_relief_models[mv->id().id];
        if (!model) {
            model = std::make_shared<GLModel>();
            model->init_from(mv->mesh().its);
        }
        const Transform3d trafo = get_volume_world_matrix(m_parent.get_selection(), mo, mv, volume_id);
        render_glmodel(*model, {0.35f, 0.35f, 0.4f, 0.6f}, camera.get_view_matrix() * trafo, camera.get_projection_matrix());
    }
}

void GLGizmoMmuSegmentation::texture_paint_to_relief(const std::vector<const TriangleSelector *> &sources, EnforcerBlockerType state, const std::string &name)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    // the painted facets of every part, in object coordinates
    indexed_triangle_set surface;
    int part = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        if (++part >= int(sources.size()))
            break;
        if (sources[part] == nullptr)
            continue;
        indexed_triangle_set its = sources[part]->get_facets_strict(state); // no T-joints: the shell stays closed
        if (its.indices.empty())
            continue;
        its_compactify_vertices(its);
        its_transform(its, mv->get_matrix());
        its_merge(surface, its);
    }
    its_merge_vertices(surface);
    its_remove_degenerate_faces(surface);
    its_compactify_vertices(surface);
    if (surface.indices.empty()) {
        wxGetApp().plater()->get_notification_manager()->push_notification(_u8L("Nothing to turn into relief: paint the model with that filament first."));
        return;
    }

    // Offset the painted surface along its normals: engraved = from just outside down to 'depth',
    // raised = from just inside up to 'depth'. The shell follows the surface, curves included.
    const int    instance_idx = std::max(0, m_parent.get_selection().get_instance_idx());
    const double scale        = std::max(1e-3, mo->instances[instance_idx]->get_scaling_factor().mean());
    const double depth        = std::max(0.05, double(m_relief_depth)) / scale;
    const double skin         = 0.2 / scale; // overlap with the part, so the result merges / cuts cleanly
    const double lo = m_relief_raised ? -skin : -depth, hi = m_relief_raised ? depth : skin;

    const size_t       nv = surface.vertices.size();
    std::vector<Vec3d> normal(nv, Vec3d::Zero()), face_normal(surface.indices.size(), Vec3d::Zero());
    for (size_t f = 0; f < surface.indices.size(); ++f) {
        const Vec3i &t = surface.indices[f];
        const Vec3d  a = surface.vertices[t[0]].cast<double>(), b = surface.vertices[t[1]].cast<double>(), c = surface.vertices[t[2]].cast<double>();
        const Vec3d  n = (b - a).cross(c - a); // area weighted
        if (n.squaredNorm() > 0.0)
            face_normal[f] = n.normalized();
        for (int k = 0; k < 3; ++k)
            normal[t[k]] += n;
    }
    for (Vec3d &n : normal)
        if (n.squaredNorm() > 0.0)
            n.normalize();
    // at a sharp edge the averaged normal leans: push such vertices further so the walls keep their thickness
    std::vector<double> stretch(nv, 1.0);
    for (size_t f = 0; f < surface.indices.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const int v = surface.indices[f][k];
            stretch[v]  = std::max(stretch[v], 1.0 / std::clamp(normal[v].dot(face_normal[f]), 0.5, 1.0));
        }
    indexed_triangle_set shell;
    shell.vertices.reserve(2 * nv);
    for (size_t i = 0; i < nv; ++i)
        shell.vertices.emplace_back((surface.vertices[i].cast<double>() + normal[i] * hi * stretch[i]).cast<float>());
    for (size_t i = 0; i < nv; ++i)
        shell.vertices.emplace_back((surface.vertices[i].cast<double>() + normal[i] * lo * stretch[i]).cast<float>());
    std::set<std::pair<int, int>> edges;
    for (const Vec3i &t : surface.indices)
        for (int k = 0; k < 3; ++k)
            edges.emplace(t[k], t[(k + 1) % 3]);
    const int off = int(nv);
    for (const Vec3i &t : surface.indices) {
        shell.indices.emplace_back(t[0], t[1], t[2]);                   // top
        shell.indices.emplace_back(t[2] + off, t[1] + off, t[0] + off); // bottom, facing away
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            if (edges.count({b, a}))
                continue; // inner edge
            // the outline: a wall from the bottom up to the top
            shell.indices.emplace_back(a + off, b + off, b);
            shell.indices.emplace_back(a + off, b, a);
        }
    }
    if (its_volume(shell) < 0.f)
        its_flip_triangles(shell);

    update_model_object(); // keep the painting done so far
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), name);
    ModelVolume *volume = mo->add_volume(TriangleMesh(std::move(shell)), m_relief_raised ? ModelVolumeType::MODEL_PART : ModelVolumeType::NEGATIVE_VOLUME);
    volume->name        = name;
    if (m_relief_raised) // same filament as the object
        for (const ModelVolume *mv : mo->volumes)
            if (mv != volume && mv->is_model_part() && mv->config.has("extruder")) {
                volume->config.set_key_value("extruder", new ConfigOptionInt(mv->config.opt_int("extruder")));
                break;
            }
    // refresh the object list, the scene and this gizmo's painting data
    auto      &objects = *wxGetApp().obj_list()->objects();
    const auto it      = std::find(objects.begin(), objects.end(), mo);
    if (it != objects.end()) {
        const int obj_idx = int(it - objects.begin());
        wxGetApp().obj_list()->update_info_items(obj_idx);
        wxGetApp().obj_list()->reorder_volumes_and_get_selection(obj_idx);
        wxGetApp().obj_list()->changed_object(obj_idx);
    }
    m_relief_added = true;
    m_relief_models.clear();
    update_from_model_object();
    m_parent.set_as_dirty();
}

bool GLGizmoMmuSegmentation::texture_visible(const Vec3d &world) const
{
    // cast a ray from just in front of the point towards the camera; any hit means something is in the way
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_c->raycaster() == nullptr)
        return true;
    const auto &casters = m_c->raycaster()->raycasters();
    const Vec3d to_camera = -m_tex_forward;
    const Vec3d start     = world + to_camera * 0.05;
    int part = -1;
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_model_part())
            continue;
        if (++part >= int(casters.size()))
            break;
        const Transform3d inv = get_volume_world_matrix(m_parent.get_selection(), mo, mv, volume_id).inverse();
        const Vec3d       s   = inv * start;
        const Vec3d       d   = (inv * (start + to_camera) - s).normalized();
        for (const auto &hit : casters[part]->get_aabb_mesh().query_ray_hits(s, d))
            if (hit.is_hit() && hit.distance() > 1e-4)
                return false;
    }
    return true;
}

std::vector<int> GLGizmoMmuSegmentation::texture_relief_mask() const
{
    std::vector<int> mask(size_t(m_img_w) * m_img_h, -1);
    for (size_t i = 0; i < mask.size(); ++i) {
        if (!m_img_alpha.empty() && m_img_alpha[i] < 128)
            continue;
        const double lum = (0.299 * m_img_rgb[3 * i] + 0.587 * m_img_rgb[3 * i + 1] + 0.114 * m_img_rgb[3 * i + 2]) / 2.55; // 0..100
        const bool   ink = m_relief_invert ? lum >= m_relief_threshold : lum < m_relief_threshold;
        if (ink)
            mask[i] = 0;
    }
    return mask;
}

void GLGizmoMmuSegmentation::render_texture_preview() const
{
    if (m_tex_tool == TextureTool::Pattern) {
        if (m_pat_hover_mesh < 0)
            return;
        const Camera &camera = wxGetApp().plater()->get_camera();
        const auto [primary, second] = texture_pattern_filaments();
        const std::array<float, 5> key{float(m_pat_type), m_pat_scale, m_pat_angle, float(primary), float(second)};
        const bool view_turned = !camera.get_dir_right().isApprox(m_pat_hover_cam_right, 1e-4) || !camera.get_dir_up().isApprox(m_pat_hover_cam_up, 1e-4);
        if (m_pat_hover_dirty || key != m_pat_hover_key) {
            m_pat_hover_cam_right = camera.get_dir_right();
            m_pat_hover_cam_up    = camera.get_dir_up();
            m_pat_hover_key       = key;
            m_pat_hover_dirty     = false;
            texture_build_pattern_hover();
        } else if (view_turned)
            return; // rebuilt on the next mouse move, not on every frame while the view turns
        // the face as it will look: both filaments, slightly lightened to tell it is a preview
        const std::array<int, 2> filaments{primary, second};
        glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glPolygonOffset(-2.f, -2.f));
        for (int st = 0; st < 2; ++st) {
            if (!m_pat_hover_models[st].is_initialized())
                continue;
            std::array<float, 4> color = filaments[st] < int(m_extruders_colors.size()) ? m_extruders_colors[filaments[st]] :
                                                                                           std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
            for (int k = 0; k < 3; ++k)
                color[k] = 0.85f * color[k] + 0.15f;
            color[3] = 1.f;
            render_glmodel(m_pat_hover_models[st], color, camera.get_view_matrix() * m_pat_hover_trafo, camera.get_projection_matrix());
        }
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        return;
    }
    const bool relief = m_tex_tool == TextureTool::Relief;
    if ((m_tex_tool != TextureTool::Decal && !relief) || (relief && m_relief_source != 0) || !m_decal_hover || m_img_w == 0 ||
        (!relief && m_img_states.empty()))
        return;
    const Vec3d center = *m_decal_hover;
    const float tool   = float(int(m_tex_tool)) + (relief ? m_relief_threshold * 0.001f + (m_relief_invert ? 0.5f : 0.f) : 0.f);
    // rebuild only when the position (by more than a fraction of a preview cell), size, angle or tool changed:
    // every rebuild casts thousands of rays into the model
    const double moved_enough = std::max(0.02, double(m_decal_width) / 400.0);
    if ((m_decal_preview_key - center).norm() > moved_enough || m_decal_preview_params[0] != m_decal_width || m_decal_preview_params[1] != m_decal_angle ||
        m_decal_preview_params[2] != tool) {
        m_decal_preview_key       = center;
        m_decal_preview_params[0] = m_decal_width;
        m_decal_preview_params[1] = m_decal_angle;
        m_decal_preview_params[2] = tool;
        m_decal_preview.clear();
        const std::vector<int>  relief_mask = relief ? texture_relief_mask() : std::vector<int>();
        const std::vector<int> &labels      = relief ? relief_mask : m_img_states;
        const double width = std::max(1.0, double(m_decal_width)), height = width * double(m_img_h) / double(m_img_w);
        const int    step  = std::max(1, std::max(m_img_w, m_img_h) / 80);
        const int    cols = (m_img_w + step - 1) / step, rows = (m_img_h + step - 1) / step;
        const double a = m_decal_angle * M_PI / 180.0, c = std::cos(a), sn = std::sin(a);

        // Cast every grid corner onto the surface along the view, like the painting does, so the preview
        // shows the stretch on tilted faces and what is hidden.
        const ModelObject *mo      = m_c->selection_info()->model_object();
        const auto        &casters = m_c->raycaster()->raycasters();
        struct PreviewPart { const MeshRaycaster *caster; Transform3d trafo, inv; Vec3d dir; };
        std::vector<PreviewPart> parts;
        if (mo != nullptr)
            for (int volume_id = 0, part = -1; volume_id < int(mo->volumes.size()); ++volume_id)
                if (mo->volumes[volume_id]->is_model_part() && ++part < int(casters.size())) {
                    const Transform3d trafo = get_volume_world_matrix(m_parent.get_selection(), mo, mo->volumes[volume_id], volume_id);
                    const Transform3d inv   = trafo.inverse();
                    parts.push_back({casters[part], trafo, inv, (inv.linear() * m_decal_hover_forward).normalized()});
                }
        const double back = 1000.0;
        std::vector<std::optional<Vec3f>> corners(size_t(cols + 1) * (rows + 1));
        for (int gy = 0; gy <= rows; ++gy)
            for (int gx = 0; gx <= cols; ++gx) {
                const double u = (double(std::min(gx * step, m_img_w)) / m_img_w - 0.5) * width;
                const double v = (0.5 - double(std::min(gy * step, m_img_h)) / m_img_h) * height;
                const Vec3d  on_plane = center + m_decal_hover_right * (c * u - sn * v) + m_decal_hover_up * (sn * u + c * v);
                const Vec3d  origin   = on_plane - m_decal_hover_forward * back;
                double       best     = std::numeric_limits<double>::max();
                for (const PreviewPart &pp : parts) {
                    const Vec3d s   = pp.inv * origin;
                    const Vec3d &d  = pp.dir;
                    const auto  hit = pp.caster->get_aabb_mesh().query_ray_hit(s, d);
                    if (hit.is_hit()) {
                        const Vec3d  w    = pp.trafo * (s + d * hit.distance());
                        const double dist = (w - origin).norm();
                        if (dist < best) {
                            best = dist;
                            corners[size_t(gy) * (cols + 1) + gx] = (w - m_decal_hover_forward * 0.15).cast<float>();
                        }
                    }
                }
            }
        std::map<int, indexed_triangle_set> per_label;
        for (int gy = 0; gy < rows; ++gy)
            for (int gx = 0; gx < cols; ++gx) {
                const int st = labels[size_t(std::min(gy * step, m_img_h - 1)) * m_img_w + std::min(gx * step, m_img_w - 1)];
                if (st < 0)
                    continue;
                const auto &p00 = corners[size_t(gy) * (cols + 1) + gx], &p10 = corners[size_t(gy) * (cols + 1) + gx + 1];
                const auto &p01 = corners[size_t(gy + 1) * (cols + 1) + gx], &p11 = corners[size_t(gy + 1) * (cols + 1) + gx + 1];
                if (!p00 || !p10 || !p01 || !p11)
                    continue; // off the model
                indexed_triangle_set &its = per_label[st];
                const int base = int(its.vertices.size());
                its.vertices.insert(its.vertices.end(), {*p01, *p11, *p10, *p00});
                its.indices.emplace_back(base, base + 1, base + 2);
                its.indices.emplace_back(base, base + 2, base + 3);
            }
        for (auto &[st, its] : per_label) {
            auto model = std::make_shared<GLModel>();
            model->init_from(its);
            m_decal_preview.emplace_back(st, std::move(model));
        }
    }
    const Camera &camera = wxGetApp().plater()->get_camera();
    glsafe(::glDisable(GL_CULL_FACE));
    for (const auto &[st, model] : m_decal_preview) {
        std::array<float, 4> color = relief ? std::array<float, 4>{1.f, 0.55f, 0.f, 1.f} :
                                     st < int(m_extruders_colors.size()) ? m_extruders_colors[st] : std::array<float, 4>{1.f, 1.f, 1.f, 1.f};
        color[3] = 0.8f;
        render_glmodel(*model, color, camera.get_view_matrix(), camera.get_projection_matrix());
    }
    glsafe(::glEnable(GL_CULL_FACE));
}

void GLGizmoMmuSegmentation::texture_apply_relief(const Vec3d &center)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_img_w == 0)
        return;
    wxBusyCursor wait;
    // project the image's relief pixels onto scratch copies of the parts, exactly like the decal paints,
    // then turn that painting into a shell: the relief lies on the surface wherever the image lands
    std::vector<std::unique_ptr<TriangleSelector>> scratch;
    std::vector<TriangleSelector *>                targets;
    std::vector<const TriangleSelector *>          sources;
    for (const ModelVolume *mv : mo->volumes)
        if (mv->is_model_part()) {
            scratch.emplace_back(std::make_unique<TriangleSelector>(mv->mesh()));
            targets.push_back(scratch.back().get());
            sources.push_back(scratch.back().get());
        }
    texture_paint_labels(center, false, texture_relief_mask(), targets);
    texture_paint_to_relief(sources, EnforcerBlockerType(1), (m_relief_raised ? _u8L("Raised") : _u8L("Engraved")) + " - " + m_img_name);
}

void GLGizmoMmuSegmentation::texture_commit(const std::string &name)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction);
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoMmuSegmentation::texture_apply_pattern(int mesh_idx, int facet)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || mesh_idx < 0 || mesh_idx >= int(m_triangle_selectors.size()))
        return;
    // the region: the clicked face - connected triangles within the face angle, like the Fill tool
    // (already known when the preview shows it)
    int              volume_id = -1;
    std::vector<int> region;
    if (m_pat_hover_mesh == mesh_idx && m_pat_hover_angle == m_pat_face_angle && facet >= 0 && facet < int(m_pat_hover_region.size()) &&
        m_pat_hover_region[facet]) {
        region    = m_pat_hover_facets;
        volume_id = m_pat_hover_volume;
    } else {
        region = texture_face_region(mesh_idx, facet, &volume_id);
        std::sort(region.begin(), region.end());
    }
    if (region.empty() || volume_id < 0)
        return;
    const Transform3d    trafo    = get_volume_world_matrix(m_parent.get_selection(), mo, mo->volumes[volume_id], volume_id);
    TriangleSelectorGUI &selector = *m_triangle_selectors[mesh_idx];
    const PatternFrame   frame    = texture_pattern_frame(volume_id, region, trafo);
    const auto [primary_idx, second_idx] = texture_pattern_filaments();
    const auto   primary = EnforcerBlockerType(primary_idx + 1);
    const auto   second  = EnforcerBlockerType(second_idx + 1);
    const double scale   = std::max(0.2, double(m_pat_scale));
    auto state_at = [&](const Vec3d &world) { return texture_pattern_second(frame, world) ? second : primary; };

    selector.set_facets_with_subdivision(
        region,
        [&](int, const std::array<Vec3f, 3> &tri) {
            TriangleSelector::FacetSubdivisionMeasurement m;
            std::array<Vec3d, 3> w = {trafo * tri[0].cast<double>(), trafo * tri[1].cast<double>(), trafo * tri[2].cast<double>()};
            m.surface_area         = tri_area(w);
            int n_second = 0, n = 0;
            for (const Vec3f &p : tri_samples(tri)) {
                n_second += state_at(trafo * p.cast<double>()) == second;
                ++n;
            }
            m.dominant_state = 2 * n_second > n ? second : primary;
            m.error_area     = m.surface_area * double(std::min(n_second, n - n_second)) / double(n);
            // 7 samples can all land on the same colour of a triangle much bigger than the pattern:
            // treat such triangles as wrong so they keep being split until they are pattern-sized
            const double longest = std::max({(w[1] - w[0]).norm(), (w[2] - w[1]).norm(), (w[0] - w[2]).norm()});
            if (longest > 0.5 * scale)
                m.error_area = std::max(m.error_area, 0.5 * m.surface_area);
            return m;
        },
        TEX_NODE_BUDGET, 0.002, 1e-3, TEX_MAX_DEPTH);
    selector.request_update_render_data(true);
}

void GLGizmoMmuSegmentation::texture_apply_image(const Vec3d &center, bool brush, int seed_part, int seed_facet)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_img_w == 0 || m_img_h == 0 || m_extruders_colors.empty())
        return;
    texture_update_states();
    std::vector<TriangleSelector *> selectors;
    for (auto &selector : m_triangle_selectors)
        selectors.push_back(selector.get());
    texture_paint_labels(center, brush, m_img_states, selectors, seed_part, seed_facet);
    for (size_t i = 0; i < m_triangle_selectors.size(); ++i)
        if (seed_part < 0 || int(i) == seed_part) // a brush stamp only changes the part under the mouse
            m_triangle_selectors[i]->request_update_render_data(true);
    m_parent.set_as_dirty();
}

void GLGizmoMmuSegmentation::texture_paint_labels(const Vec3d &center, bool brush, const std::vector<int> &states, const std::vector<TriangleSelector *> &selectors,
                                                  int seed_part, int seed_facet) const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_img_w == 0 || m_img_h == 0 || states.size() != size_t(m_img_w) * m_img_h)
        return;
    const double width  = std::max(1.0, double(m_decal_width));
    const double height = width * double(m_img_h) / double(m_img_w);
    const double radius = double(m_cursor_radius);
    // filament of the image at a world point; -1 outside the image or transparent
    auto pixel_state = [&](const Vec3d &world) {
        const Vec2d uv = tex_uv(world, m_tex_origin, m_tex_right, m_tex_up, m_decal_angle);
        double      x  = uv.x() / width + 0.5, y = 0.5 - uv.y() / height;
        if (brush) { // the brush repeats the image
            x -= std::floor(x);
            y -= std::floor(y);
        } else if (x < 0.0 || x >= 1.0 || y < 0.0 || y >= 1.0)
            return -1;
        const int px = std::clamp(int(x * m_img_w), 0, m_img_w - 1), py = std::clamp(int(y * m_img_h), 0, m_img_h - 1);
        return states[size_t(py) * m_img_w + px];
    };
    // the brush paints coarser: a stroke repaints often and must stay responsive
    const double pixel      = width / double(m_img_w);
    const float  edge_limit = float(brush ? std::max({0.4, pixel, radius / 6.0}) : std::max({0.3, pixel, width / 200.0}));

    int part = -1;
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_model_part())
            continue;
        if (++part >= int(selectors.size()))
            break;
        if (selectors[part] == nullptr || (seed_part >= 0 && part != seed_part))
            continue;
        const Transform3d           trafo = get_volume_world_matrix(m_parent.get_selection(), mo, mv, volume_id);
        const Transform3f           trafo_f = trafo.cast<float>();
        const indexed_triangle_set &its   = mv->mesh().its;

        // faces looking at the camera, inside the image (or the brush)
        std::vector<char> paintable(its.indices.size(), 0);
        std::vector<int>  start;
        const double      reach = brush ? radius : 0.5 * std::hypot(width, height);
        auto candidate = [&](size_t f) {
            const Vec3d a = trafo * its.vertices[its.indices[f][0]].cast<double>();
            const Vec3d b = trafo * its.vertices[its.indices[f][1]].cast<double>();
            const Vec3d c = trafo * its.vertices[its.indices[f][2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            if (n.squaredNorm() <= 0.0 || n.normalized().dot(-m_tex_forward) < 0.05)
                return false;
            // closest distance of the triangle to the centre, measured across the view
            auto across = [&](const Vec3d &p) { const Vec3d d = p - center; return (d - d.dot(m_tex_forward) * m_tex_forward).norm(); };
            const double d_min = std::min({across(a), across(b), across(c), across((a + b + c) / 3.0)});
            const double size  = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});
            if (d_min > reach + size)
                return false;
            // hidden behind another part as seen from the camera: leave it alone (checked per triangle:
            // per-sample rays made the decal freeze)
            if (!brush && !texture_visible((a + b + c) / 3.0) && !texture_visible(a) && !texture_visible(b) && !texture_visible(c))
                return false;
            return true;
        };
        const std::vector<Vec3i> &neighbors = selectors[part]->get_neighbors();
        if (part == seed_part && seed_facet >= 0 && seed_facet < int(its.indices.size()) && neighbors.size() == its.indices.size()) {
            // Brush: only the surface around the triangle under the mouse can be inside the brush. Walking from it
            // keeps every stamp cheap on models with millions of triangles.
            std::vector<char> seen(its.indices.size(), 0);
            std::vector<int>  stack{seed_facet};
            seen[seed_facet] = 1;
            while (!stack.empty()) {
                const int f = stack.back();
                stack.pop_back();
                if (!candidate(size_t(f)))
                    continue;
                paintable[f] = 1;
                start.push_back(f);
                for (int k = 0; k < 3; ++k)
                    if (const int nb = neighbors[f][k]; nb >= 0 && nb < int(seen.size()) && !seen[nb]) {
                        seen[nb] = 1;
                        stack.push_back(nb);
                    }
            }
        } else
            for (size_t f = 0; f < its.indices.size(); ++f)
                if (candidate(f)) {
                    paintable[f] = 1;
                    start.push_back(int(f));
                }
        if (start.empty())
            continue;

        TriangleSelector &selector = *selectors[part];
        // paint the colours covering most of the image first, so thin details (lines, text) go on last
        std::map<int, size_t> counts;
        for (int s : states)
            if (s >= 0)
                ++counts[s];
        std::vector<std::pair<size_t, int>> order;
        for (const auto &[s, n] : counts)
            order.emplace_back(n, s);
        std::sort(order.rbegin(), order.rend());
        for (const auto &[count, s] : order) {
            selector.paint_region(
                start,
                [&](const std::array<Vec3f, 3> &tri) {
                    int hits = 0, n = 0;
                    double closest = std::numeric_limits<double>::max(); // closest sample to the brush centre
                    for (const Vec3f &p : tri_samples(tri)) {
                        const Vec3d w = trafo * p.cast<double>();
                        ++n;
                        if (brush) {
                            const Vec3d  d      = w - center;
                            const double across = (d - d.dot(m_tex_forward) * m_tex_forward).norm();
                            closest             = std::min(closest, across);
                            if (across > radius)
                                continue;
                        }
                        if (pixel_state(w) == s)
                            ++hits;
                    }
                    if (hits == n)
                        return TriangleSelector::RegionCoverage::Full;
                    if (hits > 0)
                        return TriangleSelector::RegionCoverage::Partial;
                    // No sample hit, but a big triangle can still contain the brush or pixels of this colour
                    // between its samples: keep splitting it while it is larger than the detail.
                    const Vec3d  w0 = trafo * tri[0].cast<double>(), w1 = trafo * tri[1].cast<double>(), w2 = trafo * tri[2].cast<double>();
                    const double longest = std::max({(w1 - w0).norm(), (w2 - w1).norm(), (w0 - w2).norm()});
                    if (longest <= 2.0 * edge_limit)
                        return TriangleSelector::RegionCoverage::None;
                    if (brush)
                        return closest <= radius + longest ? TriangleSelector::RegionCoverage::Partial : TriangleSelector::RegionCoverage::None;
                    // decal: does the triangle overlap the image rectangle?
                    double u0 = std::numeric_limits<double>::max(), u1 = -u0, v0 = u0, v1 = -u0;
                    for (const Vec3d &w : {w0, w1, w2}) {
                        const Vec2d uv = tex_uv(w, m_tex_origin, m_tex_right, m_tex_up, m_decal_angle);
                        u0 = std::min(u0, uv.x()); u1 = std::max(u1, uv.x());
                        v0 = std::min(v0, uv.y()); v1 = std::max(v1, uv.y());
                    }
                    const bool overlaps = u1 >= -0.5 * width && u0 <= 0.5 * width && v1 >= -0.5 * height && v0 <= 0.5 * height;
                    return overlaps ? TriangleSelector::RegionCoverage::Partial : TriangleSelector::RegionCoverage::None;
                },
                EnforcerBlockerType(s + 1), edge_limit, TEX_NODE_BUDGET, TEX_MAX_DEPTH, nullptr, &paintable, &trafo_f);
        }
    }
}

void GLGizmoMmuSegmentation::texture_load_image()
{
    wxFileDialog dlg(wxGetApp().mainframe, _L("Choose an image for the decal"), wxEmptyString, wxEmptyString,
                     _L("Images") + " (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    wxImage img;
    if (!img.LoadFile(dlg.GetPath(), wxBITMAP_TYPE_ANY) || !img.IsOk()) {
        BOOST_LOG_TRIVIAL(error) << "Texture tools: cannot load image " << dlg.GetPath().ToUTF8().data();
        return;
    }
    // keep it small: a pixel is painted as a small patch of triangles
    const int longest = std::max(img.GetWidth(), img.GetHeight());
    if (longest > TEX_MAX_IMAGE)
        img.Rescale(std::max(1, img.GetWidth() * TEX_MAX_IMAGE / longest), std::max(1, img.GetHeight() * TEX_MAX_IMAGE / longest), wxIMAGE_QUALITY_HIGH);
    m_img_w = img.GetWidth();
    m_img_h = img.GetHeight();
    m_img_rgb.assign(img.GetData(), img.GetData() + size_t(m_img_w) * m_img_h * 3);
    m_img_alpha.clear();
    if (img.HasAlpha())
        m_img_alpha.assign(img.GetAlpha(), img.GetAlpha() + size_t(m_img_w) * m_img_h);
    else if (img.HasMask()) {
        m_img_alpha.assign(size_t(m_img_w) * m_img_h, 255);
        for (int y = 0; y < m_img_h; ++y)
            for (int x = 0; x < m_img_w; ++x)
                if (img.IsTransparent(x, y))
                    m_img_alpha[size_t(y) * m_img_w + x] = 0;
    }
    m_img_name = dlg.GetFilename().ToUTF8().data();
    m_img_states.clear();
    texture_update_states();
    m_decal_preview.clear();
    m_decal_preview_key = Vec3d::Constant(std::numeric_limits<double>::max());
}

void GLGizmoMmuSegmentation::render_texture_tools()
{
    m_imgui->text(_L("Textures") + ":");
    const std::array<std::pair<TextureTool, std::string>, 5> tools = {{{TextureTool::None, _u8L("Off")},
                                                                       {TextureTool::Pattern, _u8L("Pattern fill")},
                                                                       {TextureTool::Decal, _u8L("Image decal")},
                                                                       {TextureTool::Brush, _u8L("Texture brush")},
                                                                       {TextureTool::Relief, _u8L("Image relief")}}};
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i > 0 && i != 3) // two rows
            ImGui::SameLine();
        if (ImGui::RadioButton((tools[i].second + "##tex_tool").c_str(), m_tex_tool == tools[i].first)) {
            m_tex_tool     = tools[i].first;
            m_brush_stroke = false;
            m_decal_hover.reset();
            texture_clear_pattern_hover();
        }
    }
    if (m_tex_tool == TextureTool::None)
        return;

    const float label_w = std::max(m_imgui->calc_text_size(_L("Second filament")).x, m_imgui->calc_text_size(_L("Face angle") + " (deg)").x) + m_imgui->scaled(1.f);
    std::vector<std::string> filament_names;
    for (size_t i = 0; i < m_extruders_colors.size(); ++i)
        filament_names.push_back(_u8L("Filament") + " " + std::to_string(i + 1));
    auto filament_combo = [&](const wxString &label, const char *id, int &value) {
        value = std::clamp(value, 0, std::max(0, int(filament_names.size()) - 1));
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        if (ImGui::BeginCombo(id, filament_names.empty() ? "" : filament_names[value].c_str())) {
            for (int i = 0; i < int(filament_names.size()); ++i)
                if (ImGui::Selectable(filament_names[i].c_str(), i == value))
                    value = i;
            ImGui::EndCombo();
        }
        ImGuiWrapper::pop_combo_style();
        ImGui::PopItemWidth();
    };
    auto slider = [&](const wxString &label, float &value, float min_v, float max_v, const char *fmt) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        m_imgui->bbl_slider_float_style(("##tex_" + label.ToStdString()).c_str(), &value, min_v, max_v, fmt);
        ImGui::PopItemWidth();
        // the exact value can be typed in too
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(3.5f));
        if (ImGui::BBLDragFloat(("##tex_in_" + label.ToStdString()).c_str(), &value, 0.05f, 0.0f, 0.0f, fmt))
            value = std::clamp(value, min_v, max_v);
        ImGui::PopItemWidth();
    };

    if (m_tex_tool == TextureTool::Pattern) {
        const char *types[] = {"Stripes", "Checker", "Dots", "Gradient"};
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Pattern"));
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        ImGuiWrapper::push_combo_style(m_parent.get_scale());
        ImGui::Combo("##tex_pattern", &m_pat_type, types, IM_ARRAYSIZE(types));
        ImGuiWrapper::pop_combo_style();
        ImGui::PopItemWidth();
        filament_combo(_L("Second filament"), "##tex_second", m_pat_second);
        if (m_pat_second == m_selected_extruder_idx && filament_names.size() > 1)
            m_imgui->warning_text(_L("Same as the selected filament: the next filament will be used."));
        slider(_L("Size") + " (mm)", m_pat_scale, 0.5f, 30.f, "%.1f");
        slider(_L("Angle") + " (deg)", m_pat_angle, -90.f, 90.f, "%.0f");
        slider(_L("Face angle") + " (deg)", m_pat_face_angle, 0.f, 90.f, "%.0f");
        m_imgui->text(_L("Hover to see the face, click to fill it with the selected filament and the second one."));
        m_imgui->text(_L("Shift + mouse wheel: angle.  Ctrl / Alt + mouse wheel: size."));
        return;
    }

    if (m_tex_tool == TextureTool::Relief) {
        if (ImGui::RadioButton((_u8L("From an image") + "##relief_src").c_str(), m_relief_source == 0))
            m_relief_source = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton((_u8L("From a painted colour") + "##relief_src").c_str(), m_relief_source == 1)) {
            m_relief_source = 1;
            m_decal_hover.reset();
        }
    }
    auto relief_shape = [&]() {
        slider(_L("Depth") + " (mm)", m_relief_depth, 0.1f, 5.f, "%.2f");
        if (ImGui::RadioButton((_u8L("Engraved") + "##relief").c_str(), !m_relief_raised))
            m_relief_raised = false;
        ImGui::SameLine();
        if (ImGui::RadioButton((_u8L("Raised") + "##relief").c_str(), m_relief_raised))
            m_relief_raised = true;
    };
    if (m_tex_tool == TextureTool::Relief && m_relief_source == 1) {
        filament_combo(_L("Painted colour"), "##relief_state", m_relief_state);
        relief_shape();
        m_imgui->text(_L("Paint with any tool (pen, fill, pattern, decal...), then turn that colour into relief."));
        if (!filament_names.empty() && m_imgui->button(_L("Create relief from") + " " + wxString::FromUTF8(filament_names[m_relief_state]))) {
            wxBusyCursor wait;
            std::vector<const TriangleSelector *> sources;
            for (auto &selector : m_triangle_selectors)
                sources.push_back(selector.get());
            texture_paint_to_relief(sources, EnforcerBlockerType(m_relief_state + 1),
                                    (m_relief_raised ? _u8L("Raised") : _u8L("Engraved")) + " - " + filament_names[m_relief_state]);
        }
        m_imgui->text(_L("The painting stays; erase it afterwards for a single-colour relief. Undo with Ctrl+Z."));
        return;
    }

    // decal / brush / image relief
    if (m_imgui->button(_L("Load image") + "..."))
        wxGetApp().CallAfter([this]() { texture_load_image(); }); // not from inside the render pass
    ImGui::SameLine();
    m_imgui->text(m_img_w == 0 ? _L("No image loaded") : wxString::FromUTF8(m_img_name + " (" + std::to_string(m_img_w) + "x" + std::to_string(m_img_h) + ")"));
    slider(_L("Width") + " (mm)", m_decal_width, 2.f, 300.f, "%.0f");
    slider(_L("Angle") + " (deg)", m_decal_angle, -180.f, 180.f, "%.0f");
    if (m_tex_tool == TextureTool::Relief) {
        slider(_L("Threshold") + " (%)", m_relief_threshold, 1.f, 99.f, "%.0f");
        relief_shape();
        ImGui::SameLine();
        m_imgui->bbl_checkbox(_L("Use light areas"), m_relief_invert);
        m_imgui->text(_L("Click the model to engrave / raise the orange areas (same colour). It follows the surface."));
        m_imgui->text(_L("Undo with Ctrl+Z."));
    } else if (m_tex_tool == TextureTool::Decal) {
        m_imgui->text(_L("Click the model to place the image, as seen from the current view."));
        m_imgui->text(_L("Each pixel uses the closest filament colour; transparent pixels are skipped."));
    } else {
        m_imgui->text(_L("Paint to reveal the image under the brush (Pen size above). The image repeats."));
    }
    if (m_tex_tool != TextureTool::Brush)
        m_imgui->text(_L("Shift + mouse wheel: rotate.  Ctrl / Alt + mouse wheel: resize."));
}

} // namespace GUI
} // namespace Slic3r
