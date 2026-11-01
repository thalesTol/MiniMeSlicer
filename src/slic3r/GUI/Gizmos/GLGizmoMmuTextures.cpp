// SliceOfMe: texture tools for the Color painting gizmo.
//
//  - Pattern fill:  click a colour area; it is filled with stripes, checkers, dots or a dithered gradient made of
//                   the selected filament and a second one. The pattern is laid out in the current view.
//  - Image decal:   an image (PNG/JPG) is projected from the current view onto the faces looking at the camera;
//                   every pixel is matched to the nearest filament colour, transparent pixels are left alone.
//  - Texture brush: the same projection, painted with the brush where you stroke (the image repeats).
//
// Triangles are subdivided where the pattern or image needs detail (TriangleSelector::paint_region /
// set_facets_with_subdivision), so the result follows the picture instead of the original triangles.

#include "GLGizmoMmuSegmentation.hpp"

#include <cmath>
#include <set>

#include <wx/filedlg.h>
#include <wx/image.h>
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
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TexturePainting.hpp"

#include <imgui/imgui.h>

namespace Slic3r {
namespace GUI {

namespace {

constexpr size_t TEX_NODE_BUDGET = 1500000;
constexpr int    TEX_MAX_DEPTH   = 14;
constexpr int    TEX_MAX_IMAGE   = 256; // longest image side after loading, pixels

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
    if (m_tex_tool != TextureTool::None && !control_down && texture_gizmo_event(action, mouse_position))
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

bool GLGizmoMmuSegmentation::texture_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position)
{
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
        if (action == SLAGizmoEventType::LeftDown) {
            if (!texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world))
                return false; // off the model: rotate the view
            take_frame(hit_world);
            texture_apply_pattern(mesh_idx, facet, hit_mesh);
            texture_commit("Pattern fill");
            return true;
        }
        return action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::Dragging;
    case TextureTool::Decal:
        if (action == SLAGizmoEventType::LeftDown) {
            if (m_img_w == 0 || !texture_hit(mouse_position, mesh_idx, facet, hit_mesh, hit_world))
                return false;
            take_frame(hit_world);
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
            }
            texture_apply_image(hit_world, true);
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

void GLGizmoMmuSegmentation::texture_commit(const std::string &name)
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction);
    update_model_object();
    m_parent.set_as_dirty();
}

void GLGizmoMmuSegmentation::texture_apply_pattern(int mesh_idx, int facet, const Vec3f &hit_mesh)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || mesh_idx < 0 || mesh_idx >= int(m_triangle_selectors.size()))
        return;
    int volume_id = -1;
    for (int v = 0, part = -1; v < int(mo->volumes.size()); ++v)
        if (mo->volumes[v]->is_model_part() && ++part == mesh_idx) {
            volume_id = v;
            break;
        }
    if (volume_id < 0)
        return;
    const Transform3d   trafo    = get_volume_world_matrix(m_parent.get_selection(), mo, mo->volumes[volume_id], volume_id);
    TriangleSelectorGUI &selector = *m_triangle_selectors[mesh_idx];

    // the region: the connected area of the clicked colour, like the bucket fill
    selector.bucket_fill_select_triangles(hit_mesh, facet, get_clipping_plane_in_volume_coordinates(trafo), m_smart_fill_angle, true, true);
    std::set<int> region;
    for (const auto &tr : selector.get_triangles())
        if (tr.valid() && !tr.is_split() && tr.is_selected_by_seed_fill())
            region.insert(tr.source_triangle);
    selector.seed_fill_unselect_all_triangles();
    if (region.empty())
        return;

    const auto primary = EnforcerBlockerType(m_selected_extruder_idx + 1);
    const auto second  = EnforcerBlockerType(std::clamp(m_pat_second, 0, int(m_extruders_colors.size()) - 1) + 1);
    const double scale = std::max(0.2, double(m_pat_scale));

    // gradient: the extent of the region across the pattern direction
    double u_min = std::numeric_limits<double>::max(), u_max = -u_min;
    const indexed_triangle_set &its = mo->volumes[volume_id]->mesh().its;
    if (m_pat_type == 3)
        for (int f : region)
            for (int k = 0; k < 3; ++k) {
                const double u = tex_uv(trafo * its.vertices[its.indices[f][k]].cast<double>(), m_tex_origin, m_tex_right, m_tex_up, m_pat_angle).x();
                u_min = std::min(u_min, u);
                u_max = std::max(u_max, u);
            }
    auto state_at = [&](const Vec3d &world) {
        const Vec2d uv = tex_uv(world, m_tex_origin, m_tex_right, m_tex_up, m_pat_angle);
        bool use_second = false;
        switch (m_pat_type) {
        case 0: use_second = (int(std::floor(uv.x() / scale)) & 1) != 0; break; // stripes
        case 1: use_second = ((int(std::floor(uv.x() / scale)) + int(std::floor(uv.y() / scale))) & 1) != 0; break; // checker
        case 2: { // dots
            const Vec2d cell((std::floor(uv.x() / scale) + 0.5) * scale, (std::floor(uv.y() / scale) + 0.5) * scale);
            use_second = (uv - cell).norm() < 0.35 * scale;
            break;
        }
        default: { // gradient: fine stripes whose width grows across the region
            const double t    = u_max > u_min ? std::clamp((uv.x() - u_min) / (u_max - u_min), 0.0, 1.0) : 0.5;
            const double frac = uv.y() / scale - std::floor(uv.y() / scale);
            use_second        = frac < t;
            break;
        }
        }
        return use_second ? second : primary;
    };

    const std::vector<int> facets(region.begin(), region.end());
    selector.set_facets_with_subdivision(
        facets,
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
            return m;
        },
        TEX_NODE_BUDGET, 0.002, 1e-4, TEX_MAX_DEPTH);
    selector.request_update_render_data(true);
}

void GLGizmoMmuSegmentation::texture_apply_image(const Vec3d &center, bool brush)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_img_w == 0 || m_img_h == 0 || m_extruders_colors.empty())
        return;

    // image pixels -> filament (nearest colour), -1 = transparent
    std::vector<int> states(size_t(m_img_w) * m_img_h, -1);
    for (size_t i = 0; i < states.size(); ++i) {
        if (!m_img_alpha.empty() && m_img_alpha[i] < 128)
            continue;
        const std::array<size_t, 3> rgb = {m_img_rgb[3 * i], m_img_rgb[3 * i + 1], m_img_rgb[3 * i + 2]};
        double best = std::numeric_limits<double>::max();
        for (size_t f = 0; f < m_extruders_colors.size() && f < size_t(EnforcerBlockerType::ExtruderMax); ++f) {
            const double d = compute_delta_e(rgb, m_extruders_colors[f]);
            if (d < best) {
                best      = d;
                states[i] = int(f);
            }
        }
    }
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
    const float edge_limit = float(std::max(0.25, width / double(m_img_w)));

    int part = -1;
    for (int volume_id = 0; volume_id < int(mo->volumes.size()); ++volume_id) {
        const ModelVolume *mv = mo->volumes[volume_id];
        if (!mv->is_model_part())
            continue;
        if (++part >= int(m_triangle_selectors.size()))
            break;
        const Transform3d           trafo = get_volume_world_matrix(m_parent.get_selection(), mo, mv, volume_id);
        const Transform3f           trafo_f = trafo.cast<float>();
        const indexed_triangle_set &its   = mv->mesh().its;

        // faces looking at the camera, inside the image (or the brush)
        std::vector<char> paintable(its.indices.size(), 0);
        std::vector<int>  start;
        const double      reach = brush ? radius : 0.5 * std::hypot(width, height);
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const Vec3d a = trafo * its.vertices[its.indices[f][0]].cast<double>();
            const Vec3d b = trafo * its.vertices[its.indices[f][1]].cast<double>();
            const Vec3d c = trafo * its.vertices[its.indices[f][2]].cast<double>();
            const Vec3d n = (b - a).cross(c - a);
            if (n.squaredNorm() <= 0.0 || n.normalized().dot(-m_tex_forward) < 0.05)
                continue;
            // closest distance of the triangle to the centre, measured across the view
            auto across = [&](const Vec3d &p) { const Vec3d d = p - center; return (d - d.dot(m_tex_forward) * m_tex_forward).norm(); };
            const double d_min = std::min({across(a), across(b), across(c), across((a + b + c) / 3.0)});
            const double size  = std::max({(b - a).norm(), (c - b).norm(), (a - c).norm()});
            if (d_min > reach + size)
                continue;
            paintable[f] = 1;
            start.push_back(int(f));
        }
        if (start.empty())
            continue;

        TriangleSelectorGUI &selector = *m_triangle_selectors[part];
        std::set<int> used;
        for (int s : states)
            if (s >= 0)
                used.insert(s);
        for (int s : used) {
            selector.paint_region(
                start,
                [&](const std::array<Vec3f, 3> &tri) {
                    int hits = 0, n = 0;
                    for (const Vec3f &p : tri_samples(tri)) {
                        const Vec3d w = trafo * p.cast<double>();
                        ++n;
                        if (brush) {
                            const Vec3d d = w - center;
                            if ((d - d.dot(m_tex_forward) * m_tex_forward).norm() > radius)
                                continue;
                        }
                        hits += pixel_state(w) == s;
                    }
                    return hits == 0 ? TriangleSelector::RegionCoverage::None :
                           hits == n ? TriangleSelector::RegionCoverage::Full :
                                       TriangleSelector::RegionCoverage::Partial;
                },
                EnforcerBlockerType(s + 1), edge_limit, TEX_NODE_BUDGET, TEX_MAX_DEPTH, nullptr, &paintable, &trafo_f);
        }
        selector.request_update_render_data(true);
    }
    m_parent.set_as_dirty();
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
}

void GLGizmoMmuSegmentation::render_texture_tools()
{
    m_imgui->text(_L("Textures") + ":");
    const std::array<std::pair<TextureTool, std::string>, 4> tools = {{{TextureTool::None, _u8L("Off")},
                                                                       {TextureTool::Pattern, _u8L("Pattern fill")},
                                                                       {TextureTool::Decal, _u8L("Image decal")},
                                                                       {TextureTool::Brush, _u8L("Texture brush")}}};
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i > 0)
            ImGui::SameLine();
        if (ImGui::RadioButton((tools[i].second + "##tex_tool").c_str(), m_tex_tool == tools[i].first)) {
            m_tex_tool     = tools[i].first;
            m_brush_stroke = false;
        }
    }
    if (m_tex_tool == TextureTool::None)
        return;

    const float label_w = m_imgui->calc_text_size(_L("Second filament")).x + m_imgui->scaled(1.f);
    auto slider = [&](const wxString &label, float &value, float min_v, float max_v, const char *fmt) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        m_imgui->bbl_slider_float_style(("##tex_" + label.ToStdString()).c_str(), &value, min_v, max_v, fmt);
        ImGui::PopItemWidth();
    };

    if (m_tex_tool == TextureTool::Pattern) {
        const char *types[] = {"Stripes", "Checker", "Dots", "Gradient"};
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Pattern"));
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        ImGui::Combo("##tex_pattern", &m_pat_type, types, IM_ARRAYSIZE(types));
        ImGui::PopItemWidth();
        // second filament
        std::vector<std::string> names;
        for (size_t i = 0; i < m_extruders_colors.size(); ++i)
            names.push_back(_u8L("Filament") + " " + std::to_string(i + 1));
        m_pat_second = std::clamp(m_pat_second, 0, std::max(0, int(names.size()) - 1));
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Second filament"));
        ImGui::SameLine(label_w);
        ImGui::PushItemWidth(m_imgui->scaled(9.f));
        if (ImGui::BeginCombo("##tex_second", names.empty() ? "" : names[m_pat_second].c_str())) {
            for (int i = 0; i < int(names.size()); ++i)
                if (ImGui::Selectable(names[i].c_str(), i == m_pat_second))
                    m_pat_second = i;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        slider(_L("Size") + " (mm)", m_pat_scale, 0.5f, 30.f, "%.1f");
        slider(_L("Angle") + " (deg)", m_pat_angle, -90.f, 90.f, "%.0f");
        m_imgui->text(_L("Click a colour area to fill it with the selected filament and the second one."));
        return;
    }

    // decal / brush
    if (m_imgui->button(_L("Load image") + "..."))
        wxGetApp().CallAfter([this]() { texture_load_image(); }); // not from inside the render pass
    ImGui::SameLine();
    m_imgui->text(m_img_w == 0 ? _L("No image loaded") : wxString::FromUTF8(m_img_name + " (" + std::to_string(m_img_w) + "x" + std::to_string(m_img_h) + ")"));
    slider(_L("Width") + " (mm)", m_decal_width, 2.f, 300.f, "%.0f");
    slider(_L("Angle") + " (deg)", m_decal_angle, -180.f, 180.f, "%.0f");
    m_imgui->text(m_tex_tool == TextureTool::Decal ?
                      _L("Click the model to place the image, as seen from the current view.") :
                      _L("Paint to reveal the image under the brush (brush size above). The image repeats."));
    m_imgui->text(_L("Each pixel uses the closest filament colour; transparent pixels are skipped."));
}

} // namespace GUI
} // namespace Slic3r
