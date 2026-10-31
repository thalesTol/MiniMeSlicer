#include "FilamentColourDialog.hpp"

#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/valtext.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "FilamentBitmapUtils.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

#define PREVIEW_SIZE    wxSize(FromDIP(48), FromDIP(48))
#define BUTTON_SIZE     wxSize(FromDIP(58), FromDIP(24))
#define PADDING         FromDIP(20)
#define FONT_COLOR      wxColour("#6B6B6B")

static int alpha_to_percent(unsigned char alpha) { return int(std::lround(alpha * 100.0 / 255.0)); }
static unsigned char percent_to_alpha(int percent) { return (unsigned char) std::lround(std::clamp(percent, 0, 100) * 255.0 / 100.0); }

FilamentColourDialog::FilamentColourDialog(wxWindow *parent, const wxColour &initial)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Filament colour"),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_colour(initial.IsOk() ? initial : wxColour(0, 0, 0))
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->SetMinSize(wxSize(FromDIP(360), -1));

    // Preview + choose colour + hex code
    wxBoxSizer *colour_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, PREVIEW_SIZE, 0);
    colour_sizer->Add(m_preview, 0, wxALIGN_CENTER_VERTICAL);

    StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal));

    m_choose_btn = new Button(this, _L("Choose colour") + "...");
    m_choose_btn->SetBackgroundColor(btn_bg_white);
    m_choose_btn->SetBorderColor(wxColour(38, 46, 48));
    m_choose_btn->SetFont(Label::Body_12);
    m_choose_btn->SetMinSize(wxSize(-1, FromDIP(24)));
    m_choose_btn->SetCornerRadius(FromDIP(12));
    colour_sizer->Add(m_choose_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));

    colour_sizer->AddStretchSpacer(1);
    m_hex_input = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(96), -1), wxTE_CENTER | wxTE_PROCESS_ENTER);
    m_hex_input->GetTextCtrl()->SetFont(Label::Body_12);
    m_hex_input->GetTextCtrl()->SetMaxLength(9);
    m_hex_input->SetToolTip(_L("Colour code: #RRGGBB, or #RRGGBBAA to include opacity"));
    colour_sizer->Add(m_hex_input, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(colour_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    // Opacity slider + percent
    wxBoxSizer *opacity_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *opacity_title = new wxStaticText(this, wxID_ANY, _L("Opacity") + ": ");
    opacity_title->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    opacity_sizer->Add(opacity_title, 0, wxALIGN_CENTER_VERTICAL);
    m_opacity_slider = new wxSlider(this, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(FromDIP(180), FromDIP(25)), wxSL_HORIZONTAL);
    opacity_sizer->Add(m_opacity_slider, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    m_opacity_input = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1), wxTE_CENTER | wxTE_PROCESS_ENTER, "%");
    m_opacity_input->GetTextCtrl()->SetFont(Label::Body_12);
    m_opacity_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_DIGITS));
    opacity_sizer->Add(m_opacity_input, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));
    main_sizer->Add(opacity_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    wxStaticText *hint = new wxStaticText(this, wxID_ANY,
        _L("Only changes how the filament looks in the 3D view and preview. Below 10% it is shown as frosted white, like a clear filament."));
    hint->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    hint->Wrap(FromDIP(360));
    main_sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    // OK / Cancel
    wxBoxSizer *btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer(1);
    m_ok_btn = new Button(this, _L("OK"));
    m_ok_btn->SetBackgroundColor(btn_bg_green);
    m_ok_btn->SetBorderColor(*wxWHITE);
    m_ok_btn->SetTextColor(wxColour("#FFFFFE"));
    m_ok_btn->SetFont(Label::Body_12);
    m_ok_btn->SetMinSize(BUTTON_SIZE);
    m_ok_btn->SetCornerRadius(FromDIP(12));
    btn_sizer->Add(m_ok_btn, 0);
    m_cancel_btn = new Button(this, _L("Cancel"));
    m_cancel_btn->SetBackgroundColor(btn_bg_white);
    m_cancel_btn->SetBorderColor(wxColour(38, 46, 48));
    m_cancel_btn->SetFont(Label::Body_12);
    m_cancel_btn->SetMinSize(BUTTON_SIZE);
    m_cancel_btn->SetCornerRadius(FromDIP(12));
    btn_sizer->Add(m_cancel_btn, 0, wxLEFT, FromDIP(10));
    main_sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, PADDING);

    m_choose_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        wxColourData data;
        data.SetChooseFull(true);
        data.SetColour(wxColour(m_colour.Red(), m_colour.Green(), m_colour.Blue()));
        wxColourData result = show_sys_picker_dialog(this, data);
        const wxColour &picked = result.GetColour();
        // The system dialog drops alpha: keep the opacity chosen here.
        if (picked.IsOk() && picked != data.GetColour())
            set_colour(wxColour(picked.Red(), picked.Green(), picked.Blue(), m_colour.Alpha()), false);
    });
    m_hex_input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { apply_hex_input(); });
    m_hex_input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { apply_hex_input(); e.Skip(); });
    m_opacity_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
        set_colour(wxColour(m_colour.Red(), m_colour.Green(), m_colour.Blue(), percent_to_alpha(m_opacity_slider->GetValue())), false);
    });
    m_opacity_input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { apply_opacity_input(); });
    m_opacity_input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { apply_opacity_input(); e.Skip(); });
    m_ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        apply_hex_input();
        apply_opacity_input();
        EndModal(wxID_OK);
    });
    m_cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    set_colour(m_colour);

    SetSizer(main_sizer);
    Layout();
    main_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentColourDialog::set_colour(const wxColour &colour, bool update_slider, bool update_hex)
{
    m_colour = colour;
    const int percent = alpha_to_percent(m_colour.Alpha());
    if (update_slider)
        m_opacity_slider->SetValue(percent);
    m_opacity_input->GetTextCtrl()->ChangeValue(wxString::Format("%d", percent));
    if (update_hex)
        m_hex_input->GetTextCtrl()->ChangeValue(m_colour.GetAsString(wxC2S_HTML_SYNTAX));
    update_preview();
}

void FilamentColourDialog::update_preview()
{
    wxBitmap bmp = create_filament_bitmap({m_colour}, PREVIEW_SIZE, false);
    if (bmp.IsOk()) {
        m_preview->SetBitmap(bmp);
        m_preview->Refresh();
    }
}

void FilamentColourDialog::apply_hex_input()
{
    wxString text = m_hex_input->GetTextCtrl()->GetValue().Trim().Trim(false);
    if (!text.StartsWith("#"))
        text = "#" + text;
    wxColour parsed;
    if ((text.length() == 7 || text.length() == 9) && parsed.Set(text)) {
        // A 6-digit code keeps the current opacity.
        if (text.length() == 7)
            parsed = wxColour(parsed.Red(), parsed.Green(), parsed.Blue(), m_colour.Alpha());
        set_colour(parsed);
    } else {
        set_colour(m_colour); // restore the last valid code
    }
}

void FilamentColourDialog::apply_opacity_input()
{
    long percent = 0;
    if (m_opacity_input->GetTextCtrl()->GetValue().ToLong(&percent))
        set_colour(wxColour(m_colour.Red(), m_colour.Green(), m_colour.Blue(), percent_to_alpha(int(percent))));
    else
        set_colour(m_colour);
}

void FilamentColourDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_choose_btn->Rescale();
    m_ok_btn->Rescale();
    m_cancel_btn->Rescale();
    update_preview();
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
