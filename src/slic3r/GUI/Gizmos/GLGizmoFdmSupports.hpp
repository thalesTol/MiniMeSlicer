#ifndef slic3r_GLGizmoFdmSupports_hpp_
#define slic3r_GLGizmoFdmSupports_hpp_

#include "GLGizmoPainterBase.hpp"
//BBS
#include "libslic3r/Print.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

#include <boost/thread.hpp>

#include <array>
#include <chrono>
#include <optional>

namespace Slic3r::GUI {

class GLGizmoFdmSupports : public GLGizmoPainterBase
{
public:
    GLGizmoFdmSupports(GLCanvas3D& parent, unsigned int sprite_id);
    void data_changed(bool is_serializing) override;
    void render_painter_gizmo() const override;

    //BBS: add edit state
    enum EditState {
        state_idle = 0,
        state_generating = 1,
        state_ready
    };

    //BBS
    bool on_key_down_select_tool_type(int keyCode);

    // SliceOfMe: the plate zones and the support base brush
    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    // the canvas draws the plate (and every object on it) while the plate itself is being painted
    bool shows_plate() const { return plate_mode(); }

    std::string get_icon_filename(bool b_dark_mode) const override;

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    // SliceOfMe: the plate zones and guides, drawn after the plate itself
    void on_render() override { render_plate_zones(); }
    std::string on_get_name() const override;
    std::string on_get_name_str() override { return "Supports Painting"; }

    // BBS
    void render_triangles(const Selection& selection) const override;
    void on_set_state() override;
    void show_tooltip_information(float caption_max, float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return "Entering Paint-on supports"; }
    std::string get_gizmo_leaving_text() const override { return "Leaving Paint-on supports"; }
    std::string get_action_snapshot_name() override { return "Paint-on supports editing"; }

    // SliceOfMe: the support base brush paints its own state; the plate brushes hide the pen
    EnforcerBlockerType get_left_button_state_type() const override;
    EnforcerBlockerType get_right_button_state_type() const override;
    bool                cursor_hidden() const override { return plate_mode(); }

    // BBS
    wchar_t                           m_current_tool = 0;

private:
    bool on_init() override;

    //BBS: remove const
    void update_model_object() override;
    //BBS: add logic to distinguish the first_time_update and later_update
    void update_from_model_object(bool first_update) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    void select_facets_by_angle(float threshold, bool block);
    // BBS
    int get_selection_support_threshold_angle();

    int m_support_threshold_angle = -1;

    //BBS: add support preview logic
    void init_print_instance();
    void update_support_volumes();
    void invalid_support_volumes(bool invalid_step = false);
    bool need_regenerate_support_volumes();
    void generate_support_volume();
    void run_thread();
    float m_angle_threshold_deg = 40.f;
    bool m_volume_valid = false;


    GLVolume *m_support_volume = NULL;
    mutable bool m_volume_ready = false;
    bool m_is_tree_support = false;
    bool m_cancel = false;
    size_t m_object_id;
    std::vector<ObjectBase::Timestamp> m_volume_timestamps;
    PrintInstance m_print_instance;
    mutable EditState m_edit_state;
    //thread
    boost::thread   m_thread;
    // Mutex and condition variable to synchronize m_thread with the UI thread.
    std::mutex      m_mutex;
    int m_generate_count;

    // SliceOfMe: what the brush paints - supports on the object (enforce / block), a support base other objects'
    // supports may rest on, or plate zones where supports may not / may only start
    // SupportBase (painted on an object's facets) is still read by the slicer but no longer offered: the plate
    // area "stand on objects" (PlateLand) replaces it and also forces the supports onto the object
    enum class PaintTarget { Supports, SupportBase, PlateBlock, PlateAllow, PlateLand };
    enum class PlateBrush { Circle, Square, Bucket };
    PaintTarget m_paint_target{PaintTarget::Supports};
    PlateBrush  m_plate_brush{PlateBrush::Circle};
    float       m_plate_brush_size{10.f}; // mm across
    static bool is_plate_target(PaintTarget t) { return t == PaintTarget::PlateBlock || t == PaintTarget::PlateAllow || t == PaintTarget::PlateLand; }
    bool plate_mode() const { return is_plate_target(m_paint_target); }
    // the zones of the current plate as a grid of cells (0 nothing, 1 blocked, 2 allowed, 3 stand on objects),
    // plate coordinates
    struct PlateRaster
    {
        Vec2d                origin{Vec2d::Zero()};
        double               cell{0.5};
        int                  w{0}, h{0};
        std::vector<uint8_t> cells;
    };
    PlateRaster                  m_plate;
    int                          m_plate_index{-1};
    static constexpr int         PLATE_ZONES = 3;    // blocked, allowed, stand on objects
    std::vector<Vec2ds>          m_plate_loaded[PLATE_ZONES];  // what the plate config held when the raster was made
    bool                         m_plate_stroke{false};
    uint8_t                      m_plate_stroke_value{0};
    std::optional<Vec2d>         m_plate_last, m_plate_hover;
    mutable std::array<GLModel, PLATE_ZONES> m_plate_models;
    mutable bool                 m_plate_models_dirty{true};
    mutable GLModel              m_plate_cursor_model;
    mutable std::array<float, 2> m_plate_cursor_key{-1.f, -1.f};

    void       plate_sync_from_config();
    void       plate_commit();
    bool       plate_point(const Vec2d &mouse_position, Vec2d &plate_pt) const;
    void       plate_stamp(const Vec2d &p, uint8_t value);
    void       plate_stroke_stamp(const Vec2d &p);
    void       plate_bucket(const Vec2d &p, uint8_t value);
    ExPolygons plate_areas(uint8_t value) const;
    void       render_plate_zones() const;
    void       render_plate_panel(float label_width, float slider_width, float drag_left_width);

    // what helps deciding where to paint: the other objects, the plate under the overhangs (blue) and where the
    // supports started in the last slice (orange)
    void       plate_show_scene(bool show);
    void       plate_update_guides() const;
    mutable std::vector<uint8_t> m_shadow_cells;     // same grid as m_plate: 1 = under an overhang
    mutable size_t               m_shadow_key{0};
    mutable size_t               m_support_base_key{0};
    mutable GLModel              m_shadow_model, m_support_base_model;
    mutable double               m_shadow_area{0.}, m_support_base_area{0.};
    mutable GLModel              m_support_walls_model;  // the supports of the last slice, see-through
    mutable bool                 m_supports_old{false};  // the plate changed since: they are recalculated on the next slice
    bool                         m_show_supports{true};
    // "stand on objects" is painted on the objects' surfaces: ray casters of the meshes on the plate (built when
    // first needed, dropped when leaving the plate tools)
    mutable std::vector<std::pair<std::shared_ptr<const TriangleMesh>, std::unique_ptr<MeshRaycaster>>> m_scene_casters;
    bool       plate_point_on_objects(const Vec2d &mouse_position, Vec2d &plate_pt) const;
    mutable double             m_last_hit_z{0.};  // height of the brush (on an object, or 0 on the plate)
    mutable std::vector<float> m_top_z;           // same grid as m_plate: height of the top surface, 0 on the plate
    // the objects changed: "stand on objects" left on the bare plate (object deleted or moved away) is removed
    mutable bool               m_land_prune_pending{false};
    void                       plate_prune_land();
    // the bucket on an object ("stand on objects"): spreads over neighbouring spots whose height differs at most this
    float                      m_bucket_tolerance{1.f};
    // the upward facing triangles of the objects on the plate (plate coordinates) and the grid cell of their centre:
    // "stand on objects" is drawn on the real surfaces
    struct UpTriangle
    {
        Vec3f    a, b, c;
        uint32_t cell;
        int      obj_idx{-1}, vol_idx{-1}, facet{-1}; // the triangle in the model
    };
    // "stand on objects" is paint on the objects' own surfaces (SUPPORT_LANDING_STATE), so it moves, turns, saves and
    // undoes with them. The edited object is painted through the tool's own selectors, the other objects of the plate
    // through selectors made here while painting.
    struct LandPart
    {
        int                               obj_idx{-1}, vol_idx{-1};
        std::unique_ptr<TriangleSelector> selector;
        bool                              dirty{false};
    };
    mutable std::vector<LandPart> m_land_parts;
    bool                          m_land_edited_dirty{false};
    mutable size_t                m_land_render_key{0};
    mutable double                m_land_area{0.};
    TriangleSelector *land_selector(int obj_idx, int vol_idx, bool create) const;
    void              land_mark_dirty(int obj_idx, int vol_idx);
    bool              land_paint_at(const Vec2d &plate_pt, double radius, EnforcerBlockerType state);
    void              land_set_facets_in_cells(const std::vector<uint8_t> &marked, EnforcerBlockerType state);
    void              land_commit();
    size_t            land_key() const;
    mutable std::vector<UpTriangle> m_up_triangles;
    mutable GLModel                 m_land_surface_model;
    mutable bool                    m_land_surface_dirty{true};
    mutable std::chrono::steady_clock::time_point m_land_surface_built;

    // This map holds all translated description texts, so they can be easily referenced during layout calculations
    // etc. When language changes, GUI is recreated and this class constructed again, so the change takes effect.
    std::map<std::string, wxString> m_desc;
};



} // namespace Slic3r::GUI


#endif // slic3r_GLGizmoFdmSupports_hpp_
