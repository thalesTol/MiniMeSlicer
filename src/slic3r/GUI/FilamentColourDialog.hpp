#ifndef slic3r_GUI_FilamentColourDialog_hpp_
#define slic3r_GUI_FilamentColourDialog_hpp_

#include "GUI_Utils.hpp"
#include <wx/colour.h>

class wxStaticBitmap;
class wxSlider;
class Button;
class TextInput;

namespace Slic3r { namespace GUI {

// SliceOfMe: colour picker for a custom filament colour with an opacity slider.
// The result keeps its alpha, so GetAsString(wxC2S_HTML_SYNTAX) gives #RRGGBBAA
// when the colour is not fully opaque.
class FilamentColourDialog : public DPIDialog
{
public:
    FilamentColourDialog(wxWindow *parent, const wxColour &initial);

    wxColour GetColour() const { return m_colour; }

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    void set_colour(const wxColour &colour, bool update_slider = true, bool update_hex = true);
    void update_preview();
    void apply_hex_input();
    void apply_opacity_input();

    wxColour        m_colour;
    wxStaticBitmap *m_preview{nullptr};
    TextInput      *m_hex_input{nullptr};
    wxSlider       *m_opacity_slider{nullptr};
    TextInput      *m_opacity_input{nullptr};
    Button         *m_choose_btn{nullptr};
    Button         *m_ok_btn{nullptr};
    Button         *m_cancel_btn{nullptr};
};

}} // namespace Slic3r::GUI

#endif
