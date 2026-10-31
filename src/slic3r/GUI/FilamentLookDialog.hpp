#ifndef slic3r_GUI_FilamentLookDialog_hpp_
#define slic3r_GUI_FilamentLookDialog_hpp_

#include "GUI_Utils.hpp"
#include "EncodedFilament.hpp"

#include <wx/colour.h>
#include <vector>

class wxBitmapButton;
class wxScrolledWindow;
class wxSimplebook;
class wxSlider;
class wxStaticBitmap;
class wxStaticText;
class wxBoxSizer;
class Button;
class TextInput;

namespace Slic3r { namespace GUI {

// SliceOfMe: one window to choose how a filament looks, for official and third-party filaments.
// Tabs: Solid, Gradient, Multi-color, Translucent, Glow, with a filter box over every swatch.
class FilamentLookDialog : public DPIDialog
{
public:
    // fila_id: filament_id of the preset, used to list official colours (may be empty).
    // glow: current glow colour, wxNullColour when the filament does not glow.
    FilamentLookDialog(wxWindow *parent, const wxString &fila_id, const FilamentColor &current, const wxColour &glow, const std::string &fila_type);
    ~FilamentLookDialog() override;

    // Colours with alpha; GRADIENT_CLR / MULTI_CLR when more than one colour.
    const FilamentColor &GetFilamentColor() const { return m_color; }
    // wxNullColour when the filament does not glow.
    wxColour GetGlowColour() const { return m_glow; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    enum Page { pgSolid = 0, pgGradient, pgMulti, pgTranslucent, pgGlow, pgCount };

    struct Swatch
    {
        wxBitmapButton *button{nullptr};
        wxBoxSizer     *grid{nullptr}; // grid the button lives in, for re-layout after filtering
        FilamentColor   color;
        wxColour        glow;          // set for glow presets only
        bool            is_glow{false};
        wxString        name;          // shown in the tooltip and matched by the filter
        wxString        code;          // official colour code, also matched by the filter
    };

    wxScrolledWindow *create_page(Page page);
    wxBoxSizer       *add_section(wxWindow *page, wxBoxSizer *page_sizer, const wxString &title, const wxString &hint = wxEmptyString);
    wxBoxSizer       *add_grid(wxWindow *page, wxBoxSizer *page_sizer);
    void              add_swatch(wxWindow *page, wxBoxSizer *grid, const FilamentColor &color, const wxString &name, const wxString &code = wxEmptyString);
    void              add_glow_swatch(wxWindow *page, wxBoxSizer *grid, const wxColour &glow, const wxString &name);
    void              add_official_swatches(wxWindow *page, wxBoxSizer *page_sizer, Page page_kind);
    void              add_stop_editor(wxWindow *page, wxBoxSizer *page_sizer, Page page_kind);
    void              rebuild_stop_editors();

    void select_page(Page page);
    void select_swatch(size_t idx);
    void apply_filter(const wxString &text);
    void set_color(const FilamentColor &color);
    void update_preview();
    void update_opacity_controls();
    wxBitmap swatch_bitmap(const FilamentColor &color, const wxColour &glow, const wxSize &size, bool selected) const;
    wxString describe() const;

    // Picks one colour with the system dialog; keeps alpha of 'initial'. Returns wxNullColour on cancel.
    wxColour pick_colour(const wxColour &initial);

    FilamentColor m_color;
    wxColour      m_glow;
    std::vector<wxColour> m_stops; // colours being edited on the Gradient / Multi-color pages

    FilamentColorCodeQuery *m_query{nullptr};
    FilamentColorCodes     *m_official{nullptr};
    wxString                m_cur_name;

    std::vector<Swatch> m_swatches;
    int                 m_selected{-1};

    wxStaticBitmap *m_preview{nullptr};
    wxStaticText   *m_title{nullptr};
    wxStaticText   *m_subtitle{nullptr};
    TextInput      *m_filter{nullptr};
    std::vector<Button *> m_tab_buttons;
    wxSimplebook   *m_book{nullptr};
    std::vector<wxScrolledWindow *> m_pages;
    std::vector<wxStaticText *>     m_no_match; // per page "nothing matches" label

    // Gradient / Multi-color stop editors (one per page)
    wxBoxSizer *m_stop_rows[pgCount]{};
    wxWindow   *m_stop_parents[pgCount]{};

    // Translucent page
    wxSlider       *m_opacity_slider{nullptr};
    wxStaticText   *m_opacity_label{nullptr};

    Button *m_ok_btn{nullptr};
    Button *m_cancel_btn{nullptr};
};

}} // namespace Slic3r::GUI

#endif
