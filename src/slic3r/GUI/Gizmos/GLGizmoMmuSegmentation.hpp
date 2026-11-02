#ifndef slic3r_GLGizmoMmuSegmentation_hpp_
#define slic3r_GLGizmoMmuSegmentation_hpp_

#include <optional>
#include <map>
#include <array>
#include <limits>

#include "GLGizmoPainterBase.hpp"

namespace Slic3r::GUI {

class GLMmSegmentationGizmo3DScene
{
public:
    GLMmSegmentationGizmo3DScene() = delete;

    explicit GLMmSegmentationGizmo3DScene(size_t triangle_indices_buffers_count)
    {
    }

    virtual ~GLMmSegmentationGizmo3DScene() { release_geometry(); }

    [[nodiscard]] inline bool has_VBOs(size_t triangle_indices_idx) const
    {
        assert(triangle_indices_idx < this->triangle_patches.size());
        return this->triangle_indices_VBO_ids[triangle_indices_idx] != 0;
    }

    // Release the geometry data, release OpenGL VBOs.
    void release_geometry();
    // Finalize the initialization of the geometry, upload the geometry to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_vertices();
    // Finalize the initialization of the indices, upload the indices to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_triangle_indices();

    void clear()
    {
        this->vertices.clear();
        // BBS
        this->triangle_indices_VBO_ids.clear();
        this->triangle_indices_sizes.clear();

        for (TrianglePatch& patch : this->triangle_patches)
            patch.triangle_indices.clear();
        this->triangle_patches.clear();
    }

    void render(size_t triangle_indices_idx) const;

    std::vector<float>            vertices;
    //std::vector<std::vector<int>> triangle_indices;

    // BBS
    std::vector<TrianglePatch>    triangle_patches;

    // When the triangle indices are loaded into the graphics card as Vertex Buffer Objects,
    // the above mentioned std::vectors are cleared and the following variables keep their original length.
    std::vector<size_t> triangle_indices_sizes;

    // IDs of the Vertex Array Objects, into which the geometry has been loaded.
    // Zero if the VBOs are not sent to GPU yet.
    unsigned int              vertices_VBO_id{0};
    std::vector<unsigned int> triangle_indices_VBO_ids;
};

class GLGizmoMmuSegmentation : public GLGizmoPainterBase
{
public:
    GLGizmoMmuSegmentation(GLCanvas3D& parent, unsigned int sprite_id);
    ~GLGizmoMmuSegmentation() override = default;
    void data_changed(bool is_serializing) override;
    void render_painter_gizmo() const override;
    void render_non_manifold_edges() const;
    void render_triangles(const Selection& selection) const override;

    // TriangleSelector::serialization/deserialization has a limit to store 19 different states.
    // EXTRUDER_LIMIT + 1 states are used to storing the painting because also uncolored triangles are stored.
    // When increasing EXTRUDER_LIMIT, it needs to ensure that TriangleSelector::serialization/deserialization
    // will be also extended to support additional states, requiring at least one state to remain free out of 19 states.
    static const constexpr size_t EXTRUDERS_LIMIT = 16;

    struct GradientInfo {
        bool is_gradient = false;
        std::array<float, 4> color_from = {0.5f, 0.5f, 0.5f, 1.0f};
        std::array<float, 4> color_to   = {0.5f, 0.5f, 0.5f, 1.0f};
    };

    const float get_cursor_radius_min() const override { return CursorRadiusMin; }

    // BBS
    bool on_number_key_down(int number);
    bool on_key_down_select_tool_type(int keyCode);

    std::string get_icon_filename(bool is_dark_mode) const override;

protected:
    // BBS
    void                 set_painter_gizmo_data(const Selection &selection) override;
    std::array<float, 4> get_cursor_hover_color() const override;
    void on_set_state() override;

    EnforcerBlockerType get_left_button_state_type() const override { return EnforcerBlockerType(m_selected_extruder_idx + 1); }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    std::string on_get_name_str() override { return "Color Painting"; }
    void show_tooltip_information(float caption_max, float x, float y);
    bool on_is_selectable() const override;
    bool on_is_activable() const override;
    void on_load(cereal::BinaryInputArchive &ar) override;
    void on_save(cereal::BinaryOutputArchive &ar) const override;
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    void        clear_parent_paint_outline_volumes() const;
    std::string get_gizmo_entering_text() const override { return "Entering color painting"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving color painting"; }
    std::string get_action_snapshot_name() override { return "Color painting editing"; }

    // BBS
    size_t                            m_selected_extruder_idx = 0;
    std::vector<std::array<float, 4>> m_extruders_colors;
    std::vector<GradientInfo> m_gradient_info;
    std::vector<int>                  m_volumes_extruder_idxs;

    // BBS
    static const constexpr float      CursorRadiusMin = 0.1f; // cannot be zero

    // SliceOfMe: texture tools (GLGizmoMmuTextures.cpp)
    //  - Pattern fill: click a colour area, it is filled with stripes / checkers / dots / a dithered gradient
    //    of the selected filament and a second one.
    //  - Image decal: an image projected from the current view onto the model, matched to the filaments.
    //  - Texture brush: like the decal, but painted with the brush where you stroke.
    //  - Image relief:  the image's dark (or light) areas engraved into the surface or raised from it, same colour.
    //    The relief can also be made from any painted colour (pen, fill, pattern, decal...): it follows the surface.
    enum class TextureTool { None, Pattern, Decal, Brush, Relief };
    TextureTool              m_tex_tool{TextureTool::None};
    int                      m_pat_type{0};      // 0 stripes, 1 checker, 2 dots, 3 gradient
    float                    m_pat_scale{4.f};   // mm
    float                    m_pat_angle{0.f};   // degrees
    int                      m_pat_second{1};    // second filament, 0-based
    float                    m_pat_face_angle{30.f}; // faces bent up to this angle belong to the clicked face
    int                      m_pat_hover_mesh{-1};   // face highlighted under the mouse
    std::vector<char>        m_pat_hover_region;     // per facet of that part: in the highlighted face
    std::vector<int>         m_pat_hover_facets;     // the same, as a sorted list
    int                      m_pat_hover_volume{-1};
    float                    m_pat_hover_angle{-1.f};
    Transform3d              m_pat_hover_trafo{Transform3d::Identity()};
    // a light copy of the face showing the pattern (primary, second filament): the painting is not touched while hovering
    mutable std::array<GLModel, 2> m_pat_hover_models;
    mutable bool             m_pat_hover_dirty{false};
    mutable std::array<float, 5> m_pat_hover_key{-1.f, -1.f, -1.f, -1.f, -1.f}; // type, size, angle, filaments it was built with
    mutable Vec3d            m_pat_hover_cam_right{Vec3d::Zero()}, m_pat_hover_cam_up{Vec3d::Zero()};
    // where the pattern lies on a face: texture frame, plus the face's extent across it (for the gradient)
    struct PatternFrame { Vec3d origin{Vec3d::Zero()}, right{Vec3d::UnitX()}, up{Vec3d::UnitY()}; double u_min{0.}, u_max{0.}; };
    int                      m_relief_source{0};     // 0 image, 1 a painted colour
    int                      m_relief_state{0};      // painted filament to turn into relief, 0-based
    bool                     m_relief_added{false};  // reload the scene when the gizmo closes
    mutable std::map<size_t, std::shared_ptr<GLModel>> m_relief_models; // negative volumes drawn in the gizmo
    std::string              m_img_name;
    int                      m_img_w{0}, m_img_h{0};
    std::vector<unsigned char> m_img_rgb;         // 3 bytes per pixel
    std::vector<unsigned char> m_img_alpha;       // 1 byte per pixel
    float                    m_decal_width{40.f}; // mm
    float                    m_decal_angle{0.f};  // degrees
    float                    m_relief_depth{0.6f};     // mm
    bool                     m_relief_raised{false};   // raised instead of engraved
    bool                     m_relief_invert{false};   // use the light areas instead of the dark ones
    float                    m_relief_threshold{50.f}; // % brightness: darker pixels are relief
    bool                     m_brush_stroke{false};
    Vec2d                    m_brush_last_mouse{Vec2d::Zero()};
    Vec3d                    m_brush_last_hit{Vec3d::Zero()};
    std::vector<int>         m_img_states;          // filament per pixel (-1 = transparent), cached
    std::vector<std::array<float, 4>> m_img_states_colors; // filament colours the cache was made with
    // decal preview under the mouse
    std::optional<Vec3d>     m_decal_hover;
    Vec3d                    m_decal_hover_right{Vec3d::UnitX()}, m_decal_hover_up{Vec3d::UnitY()}, m_decal_hover_forward{-Vec3d::UnitZ()};
    mutable std::vector<std::pair<int, std::shared_ptr<GLModel>>> m_decal_preview; // per filament
    mutable Vec3d            m_decal_preview_key{Vec3d::Constant(std::numeric_limits<double>::max())};
    mutable float            m_decal_preview_params[3]{-1.f, -1.f, -1.f}; // width, angle, tool
    Vec3d                    m_tex_origin{Vec3d::Zero()}, m_tex_right{Vec3d::UnitX()}, m_tex_up{Vec3d::UnitY()}, m_tex_forward{-Vec3d::UnitZ()};

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    bool cursor_hidden() const override { return texture_takes_mouse(); }
    bool texture_takes_mouse() const { return m_tex_tool == TextureTool::Pattern || m_tex_tool == TextureTool::Decal || (m_tex_tool == TextureTool::Relief && m_relief_source == 0); }
    void render_texture_tools();
    bool texture_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down);
    void texture_apply_relief(const Vec3d &center);
    // turns the facets of 'state' on every part into a closed shell following the surface, added as a part / negative part
    void texture_paint_to_relief(const std::vector<const TriangleSelector *> &sources, EnforcerBlockerType state, const std::string &name);
    void texture_clear_pattern_hover();
    // the clicked face: connected facets of the part within the face angle (indices of the part's mesh)
    std::vector<int> texture_face_region(int mesh_idx, int facet, int *volume_id = nullptr) const;
    // the frame depends only on the face, the view and the pattern angle, so the preview and the click agree
    PatternFrame texture_pattern_frame(int volume_id, const std::vector<int> &region, const Transform3d &trafo) const;
    bool texture_pattern_second(const PatternFrame &frame, const Vec3d &world) const; // true: the second filament there
    std::pair<int, int> texture_pattern_filaments() const;                              // 0-based primary, second
    void texture_build_pattern_hover() const;
    void render_relief_volumes() const;
    std::vector<int> texture_relief_mask() const; // 0 = relief pixel, -1 = not
    void texture_load_image();
    bool texture_hit(const Vec2d &mouse_position, int &mesh_idx, int &facet, Vec3f &hit_mesh, Vec3d &hit_world) const;
    void texture_apply_pattern(int mesh_idx, int facet);
    // seed_part / seed_facet: the brush paints outwards from the triangle under the mouse instead of testing every triangle
    void texture_apply_image(const Vec3d &center, bool brush, int seed_part = -1, int seed_facet = -1);
    // paints 'labels' (one filament per pixel, -1 = skip) projected from the texture frame into 'selectors' (one per part)
    void texture_paint_labels(const Vec3d &center, bool brush, const std::vector<int> &labels, const std::vector<TriangleSelector *> &selectors,
                              int seed_part = -1, int seed_facet = -1) const;
    void texture_commit(const std::string &name);
    void texture_update_states();
    bool texture_visible(const Vec3d &world) const; // not hidden behind another part, seen from the camera
    void render_texture_preview() const;

private:
    bool on_init() override;

    // BBS. remove const.
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update = false) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void init_model_triangle_selectors();

    // BBS
    void update_triangle_selectors_colors();
    void init_extruders_data();

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
    mutable GLModel   m_non_manifold_edges_model;
};

} // namespace Slic3r


#endif // slic3r_GLGizmoMmuSegmentation_hpp_
