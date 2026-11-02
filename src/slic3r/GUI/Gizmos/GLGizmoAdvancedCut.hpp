#ifndef slic3r_GLGizmoAdvancedCut_hpp_
#define slic3r_GLGizmoAdvancedCut_hpp_

#include "GLGizmoBase.hpp"
#include "GLGizmoRotate.hpp"
#include "FacetPicker.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include <optional>

namespace Slic3r {
enum class CutConnectorType : int;
class ModelVolume;
struct CutConnectorAttributes;

namespace GUI {
enum class SLAGizmoEventType : unsigned char;

namespace CommonGizmosDataObjects {
class ObjectClipper;
}
class PartSelection
{
public:
    PartSelection() = default;
    PartSelection(
        const ModelObject *mo, const Transform3d &cut_matrix, int instance_idx, const Vec3d &center, const Vec3d &normal, const CommonGizmosDataObjects::ObjectClipper &oc);
    PartSelection(const ModelObject *mo, int instance_idx_in);
    ~PartSelection()
    {
        m_model.clear_objects();
        for (size_t i = 0; i < m_cut_parts.size(); i++) {
            if (m_cut_parts[i].raycaster) { delete m_cut_parts[i].raycaster; }
        }
    }

    struct PartPara
    {
        GLModel        glmodel;
        MeshRaycaster* raycaster;
        bool           is_up_part;
        Transform3d    trans;
        bool           is_modifier;
    };
    void         part_render(const Vec3d *cut_center, const Vec3d *normal);
    void         toggle_selection(const Vec2d &mouse_pos);
    void         toggle_selection(int id);
    void         turn_over_selection();
    // True when the mouse ray hits a switchable cut part (tooltip hover only).
    bool         is_mouse_over_part(const Vec2d &mouse_pos) const;
    ModelObject* model_object() { return m_model.objects.front(); }
    bool         valid() const { return m_valid; }
    bool         is_one_object() const;

    const std::vector<size_t> *get_ignored_contours_ptr() const { return (valid() ? &m_ignored_contours : nullptr); }

    std::vector<Cut::Part> get_cut_parts();
    std::vector<PartPara> &get_parts() { return m_cut_parts; }
    bool                   has_modified_cut_parts();

private:
    Model                                                            m_model;
    int                                                              m_instance_idx;
    std::vector<PartPara>                                            m_cut_parts;
    std::vector<bool>                                                m_back_cut_parts_state;
    bool                                                             m_valid = false;
    std::vector<std::pair<std::vector<size_t>, std::vector<size_t>>> m_contour_to_parts; // for each contour, there is a vector of parts above and a vector of parts below
    std::vector<size_t> m_ignored_contours; // contour that should not be rendered (the parts on both sides will both be parts of the same object)

    std::vector<Vec3d>              m_contour_points; // Debugging
    std::vector<std::vector<Vec3d>> m_debug_pts;      // Debugging
    void                            add_object(const ModelObject *object);
    // MeshRaycaster hit test for cut-part preview meshes; -1 if no hit.
    int                             pick_part_id(const Vec2d &mouse_pos) const;
};

class GLGizmoAdvancedCut : public GLGizmoRotate3D
{
private:
    unsigned int m_last_active_item_imgui{0};
    double m_snap_step{1.0};
    // archived values
    Vec3d m_ar_plane_center{Vec3d::Zero()};

    // plane_center and so on
    Vec3d m_plane_center{Vec3d::Zero()};//old name:m_cut_plane_center
    Vec3d m_plane_center_drag_start{Vec3d::Zero()};
    Vec3d m_plane_drag_start{Vec3d::Zero()};
    Vec3d m_bb_center{Vec3d::Zero()};//box center
    Vec3d m_center_offset{Vec3d::Zero()};

    Vec3d m_plane_normal{Vec3d::UnitZ()}; //old namce:Vec3d m_cut_normal//m_cut_plane_normal
    Vec3d m_plane_x_direction{Vec3d::UnitY()};
    Vec3d m_clp_normal{Vec3d::Ones()};
    // data to check position of the cut palne center on gizmo activation
    Vec3d m_min_pos{Vec3d::Zero()};
    Vec3d m_max_pos{Vec3d::Zero()};

    static const double Offset;
    static const double Margin;
    static const std::array<float, 4> GrabberColor;
    static const std::array<float, 4> GrabberHoverColor;

    mutable double m_movement;
    double m_start_movement;
    double m_start_height;

    Vec3d m_rotation;

    Vec3d m_buffered_rotation;
    double m_buffered_movement;
    double m_buffered_height;

    Vec3d m_drag_pos_start;

    bool m_keep_upper;
    bool m_keep_lower;
    bool m_cut_to_parts{false};
    bool m_place_on_cut_upper{true};
    bool m_place_on_cut_lower{false};
    bool m_rotate_upper{false};
    bool m_rotate_lower{false};

    bool m_do_segment;
    double m_segment_smoothing_alpha;
    int m_segment_number;

    mutable Grabber m_move_z_grabber;
    mutable Grabber m_move_x_grabber;

    // Pick-face mode: click a triangular facet of the model to set the cut plane.
    FacetPicker m_facet_picker;

    bool m_connectors_editing{false};
    bool m_localized_cut_editing = true;
    Vec3d m_localized_cut_pos = Vec3d::Zero();
    bool m_add_connector_ok{false};
    std::vector<size_t> m_invalid_connectors_idxs;
    bool m_show_shortcuts{false};

    std::vector<std::pair<wxString, wxString>> m_connector_shortcuts;
    std::vector<std::pair<wxString, wxString>> m_cut_plane_shortcuts;
    std::vector<std::pair<wxString, wxString>> m_cut_groove_shortcuts;
    double m_label_width{150.0};
    double m_control_width{ 200.0 };
    double m_editing_window_width;

    CutMode                  m_cut_mode{CutMode::cutPlanar};
    CutConnectorType         m_connector_type;
    size_t                   m_connector_style;
    size_t                   m_connector_shape_id;

      // Dovetail para
    Groove             m_groove;
    bool               m_groove_editing{false};
    float              m_contour_width{0.4f};
    float              m_cut_plane_radius_koef{1.5f};
    float              m_shortcut_label_width{-1.f};
    bool               m_is_slider_editing_done{false};
    bool               m_hide_cut_plane{false};
    double             m_radius{0.0};
    double             m_grabber_radius{0.0};
    double             m_grabber_connection_len{0.0};
    Vec3d              m_cut_plane_start_move_pos{Vec3d::Zero()};
    bool               m_cut_plane_as_circle{false};
    std::vector<Vec3d> m_groove_vertices;
    bool               m_was_cut_plane_dragged{false};
    bool               m_was_contour_selected{false};
    bool               m_is_dragging{false};
    bool               m_auto_center_connector{false};
    std::shared_ptr<PartSelection>    m_part_selection{nullptr};
    // dragging angel in hovered axes
    double             m_rotate_angle{0.0};
    bool               m_imperial_units{false};
    BoundingBoxf3      m_bounding_box;
    BoundingBoxf3      m_transformed_bounding_box;

    float m_connector_depth_ratio{3.f};
    float m_connector_depth_ratio_tolerance{CUT_TOLERANCE};

    float m_connector_size{2.5f};
    float m_connector_size_tolerance{CUT_TOLERANCE};
    // Input params for cut with snaps
    float        m_snap_space_proportion{0.3f};
    float        m_snap_bulge_proportion{0.15f};

    TriangleMesh m_connector_mesh;

    // remember the connectors which is selected
    mutable std::vector<bool> m_selected;
    int                       m_selected_count{0};

    GLModel m_plane; // old name:PickingModel

    GLModel m_grabber_model;

    Vec3d m_cut_line_begin{Vec3d::Zero()};
    Vec3d m_cut_line_end{Vec3d::Zero()};

    GLModel m_cut_line_model;

    Transform3d m_rotate_matrix{Transform3d::Identity()};
    Transform3d m_start_dragging_m{Transform3d::Identity()};
    std::map<CutConnectorAttributes, GLModel> m_shapes;

    struct InvalidConnectorsStatistics
    {
        unsigned int outside_cut_contour;
        unsigned int outside_bb;
        bool         is_overlap;

        void invalidate()
        {
            outside_cut_contour = 0;
            outside_bb          = 0;
            is_overlap          = false;
        }
    } m_info_stats;

    //GLSelectionRectangle m_selection_rectangle;

public:
    GLGizmoAdvancedCut(GLCanvas3D& parent, unsigned int sprite_id);

    bool gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down, bool alt_down, bool control_down) override;
    bool on_key(const wxKeyEvent &evt) override;

    double get_movement() const { return m_movement; }
    void reset_rotation();
    std::string get_tooltip() const override;

    BoundingBoxf3 bounding_box() const;
    BoundingBoxf3 transformed_bounding_box(const Vec3d &plane_center, const Transform3d &rotation_m = Transform3d::Identity()) const;

    bool is_looking_forward() const;

    bool unproject_on_cut_plane(const Vec2d &mouse_pos, Vec3d &pos, Vec3d &pos_world, bool respect_contours = true);

    virtual bool apply_clipping_plane() { return m_connectors_editing; }

    BoundingBoxf3 get_bounding_box() const override;

    std::string get_icon_filename(bool b_dark_mode) const override;
    bool        wants_enter_leave_snapshots() const override { return true; }

protected:
    virtual bool on_init();
    virtual void on_load(cereal::BinaryInputArchive &ar) override;
    virtual void on_save(cereal::BinaryOutputArchive &ar) const override;
    virtual void data_changed(bool is_serializing) override;
    virtual std::string on_get_name() const;
    virtual std::string on_get_name_str() override { return "Cut"; }
    void    apply_color_clip_plane_colors();
    virtual void on_set_state();
    void         close();
    virtual bool on_is_activable() const;
    virtual CommonGizmosDataID on_get_requirements() const override;
    virtual void on_start_dragging() override;
    virtual void on_stop_dragging() override;
    virtual void update_plate_center(Axis axis_type, double projection, bool is_abs_move); // old name:dragging_grabber_move
    virtual void update_plate_normal_boundingbox_clipper(const Transform3d &rotation_tmp); // old name:dragging_grabber_rotation
    virtual void on_update(const UpdateData& data);
    virtual bool on_mouse(const wxMouseEvent &mouse_event) override;
    virtual void on_render();
    virtual void on_render_for_picking();
    virtual void on_render_input_window(float x, float y, float bottom_limit);

    void show_tooltip_information(float x, float y);

    virtual void on_enable_grabber(unsigned int id)
    {
        if (id < 3)
            m_gizmos[id].enable_grabber(0);
        else if (id == 3)
            this->enable_grabber(0);
    }

    virtual void on_disable_grabber(unsigned int id)
    {
        if (id < 3)
            m_gizmos[id].disable_grabber(0);
        else if (id == 3)
            this->disable_grabber(0);
    }

    virtual void on_set_hover_id()
    {
        for (int i = 0; i < 3; ++i)
            m_gizmos[i].set_hover_id((m_hover_id == i) ? 0 : -1);
    }

private:
    // Set while a deferred perform_cut() is queued on the event loop, so holding the button down
    // or clicking twice in one frame cannot enqueue a second cut.
    bool m_perform_cut_requested { false };

    void perform_cut(const Selection& selection);
    bool can_perform_cut() const;
    void apply_connectors_in_model(ModelObject *mo, int &dowels_count);

    bool is_selection_changed(bool alt_down, bool shift_down);
    void select_connector(int idx, bool select);

    double calc_projection(const Vec3d &drag_pos, const Linef3 &mouse_ray, const Vec3d &project_dir) const;

    Vec3d get_plane_normal() const;
    Vec3d get_plane_center() const;

    void reset_cut_plane();
    void reset_all();

    // update the connectors position so that the connectors are on the cut plane
    void put_connectors_on_cut_plane(const Vec3d &cp_normal, double cp_offset);
    void update_plane_normal();
    void update_clipper();
    void update_dovetail_preview_clip();
    // on render
    void render_cut_plane_and_grabbers();
    void on_render_rotate_gizmos();
    void render_connectors();
    void render_localized_cut_shadow();
    void render_clipper_cut();
    void render_cut_line();
    // pick-face mode
    bool apply_picked_facet();

    void clear_selection();
    void init_connector_shapes();
    void set_connectors_editing(bool connectors_editing);
    void reset_connectors();
    void update_connector_shape();
    void apply_selected_connectors(std::function<void(size_t idx)> apply_fn);
    void select_all_connectors();
    void unselect_all_connectors();
    void validate_connector_settings();
    bool add_connector(CutConnectors &connectors, const Vec2d &mouse_position);
    bool delete_selected_connectors();
    bool is_outside_of_cut_contour(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    bool is_conflict_for_connector(size_t idx, const CutConnectors &connectors, const Vec3d cur_pos);
    //deal groove
    void switch_to_mode(CutMode new_mode);
    void flip_cut_plane();
    void update_plane_model();
    void init_picking_models();
    bool has_valid_groove_shape() const;
    bool has_valid_groove() const;
    bool has_valid_contour() const;
    void reset_cut_by_contours();
    void render_flip_plane_button(bool disable_pred = false);
    void process_contours();
    void toggle_model_objects_visibility(bool show_in_3d = false);
    void deal_connector_pos_by_type(Vec3d &pos, float &height, CutConnectorType, CutConnectorStyle, bool looking_forward, bool is_edit, const Vec3d &clp_normal);
    void update_bb();
    void check_and_update_connectors_state();
    void set_center(const Vec3d &center, bool update_tbb = false);
    bool set_center_pos(const Vec3d &center_pos, bool update_tbb = false);
    void invalidate_cut_plane();
    void rotate_vec3d_around_plane_center(Vec3d &vec, const Transform3d &rotate_matrix, const Vec3d &center);
    Transform3d get_cut_matrix(const Selection &selection);
    // render input window
    void update_buffer_data();
    bool render_cut_mode_combo(double label_width,float item_width);
    void render_color_marker(float size, const ColorRGBA &color);
    void render_cut_plane_input_window(float x, float y, float bottom_limit);
    void init_connectors_input_window_data();
    void render_connectors_input_window(float x, float y, float bottom_limit);
    void render_input_window_warning() const;
    bool render_reset_button(const std::string &label_id, const std::string &tooltip) const;
    bool render_connect_type_radio_button(CutConnectorType type);

    bool render_slider_double_input(const std::string &label, float &value_in, float &tolerance_in);
    bool render_slider_double_input_by_format(const std::string &label, float &value_in, float value_min, float value_max, DoubleShowType show_type = DoubleShowType::Normal);
    bool cut_line_processing() const;
    void discard_cut_line_processing();
    bool process_cut_line(SLAGizmoEventType action, const Vec2d &mouse_position);

    // SliceOfMe: curved and shape cuts (GLGizmoAdvancedCutCurve.cpp).
    // The outline drawn on the cut plane is pushed through the object along the plane normal and the
    // object is split into the part inside (A) and outside (B) with mesh booleans. Both are sub-modes of
    // the planar mode so the libslic3r CutMode enum stays untouched.
    //  - Curved: a curvature pen. Every click adds a point and the path runs smoothly through all of them;
    //    Shift+click makes a corner, click-drag pulls out handles. Right-click stops drawing; then the line
    //    can be grabbed and bent, points dragged, and right-clicking a point removes it.
    //  - Shape: circle, ellipse, rectangle, rounded rectangle or polygon, drawn by dragging on the plane.
    // The split is previewed in the background; right-clicking a preview piece moves it between A and B.
public: // used by helpers in GLGizmoAdvancedCutCurve.cpp
    struct CurveAnchor
    {
        Vec2d p{Vec2d::Zero()};     // point in the cut plane frame, mm
        Vec2d h_in{Vec2d::Zero()};  // incoming handle, relative to p (used when 'manual')
        Vec2d h_out{Vec2d::Zero()}; // outgoing handle, relative to p (used when 'manual')
        bool  corner{false};        // sharp point, no handles
        bool  manual{false};        // handles set by the user; otherwise computed from the neighbours
    };
    enum class ShapeType : int { Circle = 0, Ellipse, Rectangle, RoundedRect, Polygon };
    struct CurveShape
    {
        ShapeType type{ShapeType::Circle};
        Vec2d     center{Vec2d::Zero()}; // plane frame, mm
        Vec2d     size{Vec2d::Zero()};   // full width / height, mm
        double    corner_radius{3.0};
        int       sides{6};
        double    rotation{0.0};         // degrees
        bool      valid() const { return size.x() > 0.1 && size.y() > 0.1; }
    };
    // Connectors on the cut wall of a curved / shape cut. The wall is the outline pushed along the plane
    // normal, so a dovetail running along that normal lets the parts slide together.
    enum class ConnType : int { Dovetail = 0, Pin, Snap, Dowel, Bowtie };
    struct CurveConnParams
    {
        ConnType type{ConnType::Dovetail};
        double   width{8.0};        // dovetail / bowtie: neck width; snap: barb width along the wall
        double   depth{3.0};        // dovetail / snap: how far it sticks into the other part; bowtie: per side
        double   angle{15.0};       // dovetail / bowtie flank angle, degrees
        bool     closed_end{false}; // dovetail: slot closed at one end, so the slide stops
        double   length{70.0};      // dovetail with closed end: % of the wall height it runs
        double   pin_diameter{5.0}; // pin / dowel
        double   pin_length{6.0};   // pin: into the other part; dowel: total length
        double   snap_height{6.0};  // barb length along the sliding direction
        double   tolerance{0.15};   // clearance on the receiving side, mm
        bool     on_part_b{false};  // the connector belongs to part B (default A)
        bool     reverse{false};    // slide in from the other side
        bool     loose{false};      // dovetail / pin / snap made as a separate piece, with holes in both parts
        bool     separate() const { return type == ConnType::Dowel || type == ConnType::Bowtie || loose; }
    };
    struct CurveConnector
    {
        double          t{0.5}; // position along the outline, 0..1 of its length
        double          z{0.0}; // position across the wall (along the plane normal, plane frame), mm
        CurveConnParams params;
    };
    struct CurveState
    {
        std::vector<CurveAnchor>    anchors;
        bool                        drawing{true};
        CurveShape                  shape;
        std::vector<CurveConnector> conns;
    };
    struct CurvePreview; // background boolean result
    struct CurvePiece;   // one connected piece of the preview, with its render model and raycaster
    // Cutters in world coordinates: part A = object inside 'a', part B = object outside 'b'. 'a' and 'b'
    // differ only by the connectors (solid on one side, hole with clearance on the other). Connectors that
    // are not straight extrusions are returned as separate solids for the caller to add (they need 3D booleans).
    struct CurveCutters
    {
        struct Op
        {
            TriangleMesh mesh;
            bool         on_a;     // applies to cutter a (else b)
            bool         unite;    // union (else subtract)
            int          conn{-1}; // connector it belongs to
        };
        struct Key
        {
            int          conn{-1};
            TriangleMesh mesh;
        };
        TriangleMesh     a, b;
        std::vector<Op>  ops;       // 3D connectors, applied in order on a background thread
        std::vector<Key> keys;      // separate connector pieces (dowels, bowtie keys, loose connectors)
        std::vector<int> bad_conns; // connectors whose shape could not be built
        std::string      error;     // why the cutters could not be built
    };
private:

    bool                     m_curve_mode{false};  // Curved or Shape mode is active
    bool                     m_shape_mode{false};  // Shape mode (subset of m_curve_mode)
    std::vector<CurveAnchor> m_curve_anchors;
    CurveShape               m_curve_shape;
    bool                     m_curve_drawing{true}; // Curved: clicks add points
    std::optional<Vec2d>     m_curve_hover;         // rubber-band point under the mouse
    int                      m_curve_drag_idx{-1};  // point being moved
    bool                     m_curve_drag_handle{false}; // pulling out the handles of a new point
    int                      m_curve_bend_span{-1}; // span being bent by dragging the line
    double                   m_curve_bend_t{0.5};   // where on that span the line was grabbed
    int                      m_shape_drag{0};       // 1 = drawing the shape, 2 = moving it
    Vec2d                    m_shape_drag_start{Vec2d::Zero()};
    std::vector<CurveState>  m_curve_undo, m_curve_redo; // drawing history, separate from the project undo
    std::vector<CurveConnector> m_curve_conns;
    CurveConnParams             m_conn;                // settings for new connectors
    bool                        m_conn_editing{false}; // clicks on the cut wall place / select connectors
    int                         m_conn_selected{-1};
    bool                        m_conn_dragging{false};
    bool                        m_conn_show_b{false};  // while editing, show part B instead of A
    std::optional<std::pair<double, double>> m_conn_hover; // (t, z) on the cut wall under the mouse: ghost connector
    bool                        m_curve_preview_failed{false}; // the last preview failed; older pieces may still be shown
    bool                        m_curve_repair_requested{false};
    std::string                 m_curve_repair_msg;    // result of the last "Repair model"

    GLModel                                  m_curve_model;
    std::shared_ptr<CurvePreview>            m_curve_preview;
    std::vector<std::shared_ptr<CurvePiece>> m_curve_pieces; // built from the preview on the UI thread
    size_t                                   m_curve_generation{0};
    size_t                                   m_curve_preview_shown{size_t(-1)};
    Transform3d                              m_curve_last_plane{Transform3d::Identity()}; // preview is redone when the plane moves

    Transform3d              curve_plane_matrix() const;
    bool                     curve_unproject(const Vec2d &mouse_pos, Vec2d &local);
    std::vector<CurveAnchor> curve_effective_anchors(const std::vector<CurveAnchor> &anchors) const; // fills automatic handles
    std::vector<Vec2d>       curve_outline(bool with_hover) const; // path or shape sampled into a polyline
    bool                     curve_build_cutters(CurveCutters &out) const;
    bool                     curve_pick_wall(const Vec2d &mouse_pos, double &t, double &z) const; // point on the cut wall
    bool                     curve_delete_selected_connector();
    void                     render_curve_connectors_panel();
    void                     curve_changed();                      // restart the background preview
    bool                     curve_preview_ready() const;   // preview matches the current outline
    bool                     curve_preview_visible() const; // some preview is shown (may be one step behind)
    void                     curve_update_pieces();                // take over a finished preview
    int                      curve_pick_piece(const Vec2d &mouse_pos) const;
    void                     curve_reset();
    bool                     curve_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down);
    bool                     shape_gizmo_event(SLAGizmoEventType action, const Vec2d &mouse_position, bool shift_down);
    void                     curve_on_mouse(const wxMouseEvent &mouse_event);
    void                     render_curve();
    void                     render_curve_panel();
    bool                     can_perform_curve_cut() const;
    void                     perform_curve_cut(const Selection &selection);
    void                     curve_push_undo(); // call before every change of the path or shape
    void                     curve_repair_model();  // in-app mesh repair of the object being cut

public:
    // Ctrl+Z / Ctrl+Y while drawing a curved or shape cut. Returns false when there is nothing to undo,
    // so the key falls through to the normal project undo.
    bool curve_undo(bool redo);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoAdvancedCut_hpp_