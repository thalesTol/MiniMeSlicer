#include "calib_dlg.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include <wx/dcgraph.h>
#include "MainFrame.hpp"
#include <string>
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace GUI {

wxBoxSizer* create_item_checkbox(wxString title, wxWindow* parent, bool* value, CheckBox*& checkbox)
{
    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    checkbox = new ::CheckBox(parent);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144, 144, 144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    checkbox->SetValue(true);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [parent, checkbox, value](wxCommandEvent& e) {
        (*value) = (*value) ? false : true;
        e.Skip();
        });

    return m_sizer_checkbox;
}

PA_Calibration_Dlg::PA_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("PA Calibration"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_plater(plater)
{
    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    Preset &printer_preset = wxGetApp().preset_bundle->printers.get_edited_preset();
    auto *extruder_types = printer_preset.config.option<ConfigOptionEnumsGeneric>("extruder_type");
    if (extruder_types) {
        for (int i = 0; i < (int) extruder_types->values.size(); ++i) {
            if ((ExtruderType) extruder_types->values[i] == ExtruderType::etBowden) {
                m_hasBowdenExtruder = true;
                m_bowdenExtruderId  = i;
                break;
            }
        }
    }

    if (m_hasBowdenExtruder) {
        wxString rbExtruderTypeChoices[] = { _L("Direct Drive"), _L("Bowden") };
        int rbExtruderTypeNChoices = sizeof(rbExtruderTypeChoices) / sizeof(wxString);
        m_rbExtruderType = new wxRadioBox(this, wxID_ANY, _L("Extruder type"), wxDefaultPosition, wxDefaultSize, rbExtruderTypeNChoices, rbExtruderTypeChoices, 2, wxRA_SPECIFY_COLS);
        m_rbExtruderType->SetSelection(0);
        v_sizer->Add(m_rbExtruderType, 0, wxLEFT | wxRIGHT | wxTOP, 5);
    }

	wxString m_rbMethodChoices[] = { _L("PA Tower"), _L("PA Line"), _L("PA Pattern") };
	int m_rbMethodNChoices = sizeof(m_rbMethodChoices) / sizeof(wxString);
	m_rbMethod = new wxRadioBox(this, wxID_ANY, _L("Method"), wxDefaultPosition, wxDefaultSize, m_rbMethodNChoices, m_rbMethodChoices, 3, wxRA_SPECIFY_COLS);
	m_rbMethod->SetSelection(0);
	v_sizer->Add(m_rbMethod, 0, wxLEFT | wxRIGHT | wxTOP, 5);

    // Settings
    //
    wxString start_pa_str = _L("Start PA: ");
    wxString end_pa_str = _L("End PA: ");
    wxString PA_step_str = _L("PA step: ");
	auto text_size = wxWindow::GetTextExtent(start_pa_str);
	text_size.IncTo(wxWindow::GetTextExtent(end_pa_str));
	text_size.IncTo(wxWindow::GetTextExtent(PA_step_str));
	text_size.x = text_size.x * 1.5;
	wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));

	auto st_size = FromDIP(wxSize(text_size.x, -1));
	auto ti_size = FromDIP(wxSize(90, -1));
    // start PA
    auto start_PA_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_pa_text = new wxStaticText(this, wxID_ANY, start_pa_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStartPA = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_tiStartPA->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

	start_PA_sizer->Add(start_pa_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    start_PA_sizer->Add(m_tiStartPA, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(start_PA_sizer);

    // end PA
    auto end_PA_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_pa_text = new wxStaticText(this, wxID_ANY, end_pa_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEndPA = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_tiEndPA->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_PA_sizer->Add(end_pa_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    end_PA_sizer->Add(m_tiEndPA, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(end_PA_sizer);

    // PA step
    auto PA_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto PA_step_text = new wxStaticText(this, wxID_ANY, PA_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiPAStep = new TextInput(this, "", "", "", wxDefaultPosition, ti_size, wxTE_CENTRE | wxTE_PROCESS_ENTER);
    m_tiPAStep->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    PA_step_sizer->Add(PA_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    PA_step_sizer->Add(m_tiPAStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(PA_step_sizer);

	settings_sizer->Add(create_item_checkbox(_L("Print numbers"), this, &m_params.print_numbers, m_cbPrintNum));
    m_cbPrintNum->SetValue(false);

    v_sizer->Add(settings_sizer);
	v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));

    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
		std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
		std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));

	m_btnStart->SetBackgroundColor(btn_bg_green);
	m_btnStart->SetBorderColor(wxColour(0, 150, 136));
	m_btnStart->SetTextColor(wxColour("#FFFFFE"));
	m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
	m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
	m_btnStart->SetCornerRadius(FromDIP(3));
	m_btnStart->Bind(wxEVT_BUTTON, &PA_Calibration_Dlg::on_start, this);
	v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    PA_Calibration_Dlg::reset_params();

    // Connect Events
    if (m_rbExtruderType)
        m_rbExtruderType->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_extruder_type_changed), NULL, this);
    m_rbMethod->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_method_changed), NULL, this);
    this->Connect(wxEVT_SHOW, wxShowEventHandler(PA_Calibration_Dlg::on_show));
    //wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
}

PA_Calibration_Dlg::~PA_Calibration_Dlg() {
    // Disconnect Events
    if (m_rbExtruderType)
        m_rbExtruderType->Disconnect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_extruder_type_changed), NULL, this);
    m_rbMethod->Disconnect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(PA_Calibration_Dlg::on_method_changed), NULL, this);
    m_btnStart->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(PA_Calibration_Dlg::on_start), NULL, this);
}

void PA_Calibration_Dlg::reset_params() {
    bool isDDE = true;
    if (m_rbExtruderType && m_rbExtruderType->GetSelection() == 1)
        isDDE = false;
    int method = m_rbMethod->GetSelection();

    m_tiStartPA->GetTextCtrl()->SetValue(wxString::FromDouble(0.0));

    switch (method) {
        case 1:
            m_params.mode = CalibMode::Calib_PA_Line;
            m_tiEndPA->GetTextCtrl()->SetValue(wxString::FromDouble(0.1));
            m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(0.002));
            m_cbPrintNum->SetValue(true);
            m_cbPrintNum->Enable(true);
            break;
        case 2:
            m_params.mode = CalibMode::Calib_PA_Pattern;
            m_tiEndPA->GetTextCtrl()->SetValue(wxString::FromDouble(0.08));
            m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(0.005));
            m_cbPrintNum->SetValue(true);
            m_cbPrintNum->Enable(false);
            break;
        default:
            m_params.mode = CalibMode::Calib_PA_Tower;
            m_tiEndPA->GetTextCtrl()->SetValue(wxString::FromDouble(0.1));
            m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(0.002));
            m_cbPrintNum->SetValue(false);
            m_cbPrintNum->Enable(false);
            break;
    }

    if (!isDDE) {
        m_tiEndPA->GetTextCtrl()->SetValue(wxString::FromDouble(1.0));

        if (m_params.mode == CalibMode::Calib_PA_Pattern) {
            m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(0.05));
        } else {
            m_tiPAStep->GetTextCtrl()->SetValue(wxString::FromDouble(0.02));
        }
    }
}

void PA_Calibration_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStartPA->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEndPA->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiPAStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!is_pa_params_valid(m_params))
        return;

    switch (m_rbMethod->GetSelection()) {
        case 1:
            m_params.mode = CalibMode::Calib_PA_Line;
            break;
        case 2:
            m_params.mode = CalibMode::Calib_PA_Pattern;
            break;
        default:
            m_params.mode = CalibMode::Calib_PA_Tower;
    }

    m_params.print_numbers = m_cbPrintNum->GetValue();

    m_params.has_bowden_extruder = m_hasBowdenExtruder;
    if (m_hasBowdenExtruder && m_rbExtruderType && m_rbExtruderType->GetSelection() == 1 && m_bowdenExtruderId >= 0)
        m_params.extruder_id = m_bowdenExtruderId;
    else
        m_params.extruder_id = 0;

    m_plater->calib_pa(m_params);
    EndModal(wxID_OK);

}
void PA_Calibration_Dlg::on_extruder_type_changed(wxCommandEvent& event) {
    PA_Calibration_Dlg::reset_params();
    event.Skip();
}
void PA_Calibration_Dlg::on_method_changed(wxCommandEvent& event) {
    PA_Calibration_Dlg::reset_params();
    event.Skip();
}

void PA_Calibration_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();
}

void PA_Calibration_Dlg::on_show(wxShowEvent& event) {
    PA_Calibration_Dlg::reset_params();
}

// Temp Calib dlg
//
enum FILAMENT_TYPE : int
{
    tPLA = 0,
    tABS_ASA,
    tPETG,
    tPCTG,
    tTPU,
    tTPU_AMS,
    tPA_CF,
    tPET_CF,
    tCustom
};

Temp_Calibration_Dlg::Temp_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Temperature calibration"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_plater(plater)
{
    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    wxBoxSizer* choice_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxString m_rbFilamentTypeChoices[] = {"PLA", "ABS/ASA", "PETG", "PCTG", "TPU", "TPU-AMS", "PA-CF", "PET-CF", _L("Custom")};
    int m_rbFilamentTypeNChoices = sizeof(m_rbFilamentTypeChoices) / sizeof(wxString);
    m_rbFilamentType = new wxRadioBox(this, wxID_ANY, _L("Filament type"), wxDefaultPosition, wxDefaultSize, m_rbFilamentTypeNChoices, m_rbFilamentTypeChoices, 2, wxRA_SPECIFY_COLS);
    m_rbFilamentType->SetSelection(0);
    m_rbFilamentType->Select(0);
    choice_sizer->Add(m_rbFilamentType, 0, wxALL, 5);
    choice_sizer->Add(FromDIP(5), 0, 0, wxEXPAND, 5);
    wxString m_rbMethodChoices[] = { _L("PA Tower"), _L("PA Line") };

    v_sizer->Add(choice_sizer);

    // Settings
    //
    wxString start_temp_str = _L("Start temp: ");
    wxString end_temp_str = _L("End temp: ");
    wxString temp_step_str = _L("Temp step: ");
    auto text_size = wxWindow::GetTextExtent(start_temp_str);
    text_size.IncTo(wxWindow::GetTextExtent(end_temp_str));
    text_size.IncTo(wxWindow::GetTextExtent(temp_step_str));
    text_size.x = text_size.x * 1.5;
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));

    auto st_size = FromDIP(wxSize(text_size.x, -1));
    auto ti_size = FromDIP(wxSize(90, -1));
    // start temp
    auto start_temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_temp_text = new wxStaticText(this, wxID_ANY, start_temp_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, std::to_string(230), "°C", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_temp_sizer->Add(start_temp_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    start_temp_sizer->Add(m_tiStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(start_temp_sizer);

    // end temp
    auto end_temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_temp_text = new wxStaticText(this, wxID_ANY, end_temp_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, std::to_string(190), "°C", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_temp_sizer->Add(end_temp_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    end_temp_sizer->Add(m_tiEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(end_temp_sizer);

    // temp step
    auto temp_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto temp_step_text = new wxStaticText(this, wxID_ANY, temp_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(5),"°C", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_tiStep->Enable(false);
    temp_step_sizer->Add(temp_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    temp_step_sizer->Add(m_tiStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(temp_step_sizer);

    v_sizer->Add(settings_sizer);
    v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
		std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
		std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));

    m_btnStart->SetBackgroundColor(btn_bg_green);
    m_btnStart->SetBorderColor(wxColour(0, 150, 136));
    m_btnStart->SetTextColor(wxColour("#FFFFFE"));
    m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetCornerRadius(FromDIP(3));
    m_btnStart->Bind(wxEVT_BUTTON, &Temp_Calibration_Dlg::on_start, this);
    v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    m_rbFilamentType->Connect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(Temp_Calibration_Dlg::on_filament_type_changed), NULL, this);
    m_btnStart->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(Temp_Calibration_Dlg::on_start), NULL, this);

    //wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();

    auto validate_text = [this](TextInput* ti){
        unsigned long t = 0;
        if(!ti->GetTextCtrl()->GetValue().ToULong(&t))
            return;
        if(t> 350 || t < 180){
            MessageDialog msg_dlg(nullptr, _L("Supported range: 180°C - 350°C"), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            if(t > 350)
                t = 350;
            else
                t = 180;
        }
        t = (t / 5) * 5;
        ti->GetTextCtrl()->SetValue(std::to_string(t));
    };

    m_tiStart->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [&](wxFocusEvent &e) {
        validate_text(this->m_tiStart);
        e.Skip();
        });

    m_tiEnd->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [&](wxFocusEvent &e) {
        validate_text(this->m_tiEnd);
        e.Skip();
        });


}

Temp_Calibration_Dlg::~Temp_Calibration_Dlg() {
    // Disconnect Events
    m_rbFilamentType->Disconnect(wxEVT_COMMAND_RADIOBOX_SELECTED, wxCommandEventHandler(Temp_Calibration_Dlg::on_filament_type_changed), NULL, this);
    m_btnStart->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(Temp_Calibration_Dlg::on_start), NULL, this);
}

void Temp_Calibration_Dlg::on_start(wxCommandEvent& event) {
    bool read_long = false;
    unsigned long start=0,end=0;
    read_long = m_tiStart->GetTextCtrl()->GetValue().ToULong(&start);
    read_long = read_long && m_tiEnd->GetTextCtrl()->GetValue().ToULong(&end);

    if (!read_long || start > 350 || end < 180  || end > (start - 5)) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nStart temp: <= 350\nEnd temp: >= 180\nStart temp >= End temp + 5)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }
    m_params.start = start;
    m_params.end = end;
    m_params.mode =CalibMode::Calib_Temp_Tower;
    m_plater->calib_temp(m_params);
    EndModal(wxID_OK);

}

void Temp_Calibration_Dlg::on_filament_type_changed(wxCommandEvent& event) {
    int selection = event.GetSelection();
    unsigned long start,end;
    switch(selection)
    {
        case tABS_ASA:
            start = 270;
            end = 230;
            break;
        case tPETG:
            start = 250;
            end = 230;
            break;
        case tPCTG:
            start = 280;
            end   = 240;
            break;
        case tTPU:
        case tTPU_AMS:
            start = 240;
            end = 210;
            break;
        case tPA_CF:
            start = 320;
            end = 280;
            break;
        case tPET_CF:
            start = 320;
            end = 280;
            break;
        case tPLA:
        case tCustom:
            start = 230;
            end = 190;
            break;
    }

    m_tiEnd->GetTextCtrl()->SetValue(std::to_string(end));
    m_tiStart->GetTextCtrl()->SetValue(std::to_string(start));
    event.Skip();
}

void Temp_Calibration_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}


// MaxVolumetricSpeed_Test_Dlg
//

MaxVolumetricSpeed_Test_Dlg::MaxVolumetricSpeed_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Max volumetric speed test"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_plater(plater)
{
    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    // Settings
    //
    wxString start_vol_str = _L("Start volumetric speed: ");
    wxString end_vol_str = _L("End volumetric speed: ");
    wxString vol_step_str = _L("step: ");
    auto text_size = wxWindow::GetTextExtent(start_vol_str);
    text_size.IncTo(wxWindow::GetTextExtent(end_vol_str));
    text_size.IncTo(wxWindow::GetTextExtent(vol_step_str));
    text_size.x = text_size.x * 1.5;
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));

    auto st_size = FromDIP(wxSize(text_size.x, -1));
    auto ti_size = FromDIP(wxSize(90, -1));
    // start vol
    auto start_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_vol_text = new wxStaticText(this, wxID_ANY, start_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, std::to_string(5), _L("mm³/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_vol_sizer->Add(start_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    start_vol_sizer->Add(m_tiStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(start_vol_sizer);

    // end vol
    auto end_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_vol_text = new wxStaticText(this, wxID_ANY, end_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, std::to_string(20), _L("mm³/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_vol_sizer->Add(end_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    end_vol_sizer->Add(m_tiEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(end_vol_sizer);

    // vol step
    auto vol_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto vol_step_text = new wxStaticText(this, wxID_ANY, vol_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(0.5), _L("mm³/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    vol_step_sizer->Add(vol_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    vol_step_sizer->Add(m_tiStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(vol_step_sizer);

    v_sizer->Add(settings_sizer);
    v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
		std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
		std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));

    m_btnStart->SetBackgroundColor(btn_bg_green);
    m_btnStart->SetBorderColor(wxColour(0, 150, 136));
    m_btnStart->SetTextColor(wxColour("#FFFFFE"));
    m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetCornerRadius(FromDIP(3));
    m_btnStart->Bind(wxEVT_BUTTON, &MaxVolumetricSpeed_Test_Dlg::on_start, this);
    v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    m_btnStart->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(MaxVolumetricSpeed_Test_Dlg::on_start), NULL, this);

    //wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
}

MaxVolumetricSpeed_Test_Dlg::~MaxVolumetricSpeed_Test_Dlg() {
    // Disconnect Events
    m_btnStart->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(MaxVolumetricSpeed_Test_Dlg::on_start), NULL, this);
}

void MaxVolumetricSpeed_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!read_double || m_params.start <= 0 || m_params.step <= 0 || m_params.end < (m_params.start + m_params.step)) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 0 step >= 0\nend >= start + step)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_params.mode = CalibMode::Calib_Vol_speed_Tower;
    m_plater->calib_max_vol_speed(m_params);
    EndModal(wxID_OK);

}

void MaxVolumetricSpeed_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}


// VFA_Test_Dlg
//

VFA_Test_Dlg::VFA_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("VFA test"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_plater(plater)
{
    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    // Settings
    //
    wxString start_str = _L("Start speed: ");
    wxString end_vol_str = _L("End speed: ");
    wxString vol_step_str = _L("step: ");
    auto text_size = wxWindow::GetTextExtent(start_str);
    text_size.IncTo(wxWindow::GetTextExtent(end_vol_str));
    text_size.IncTo(wxWindow::GetTextExtent(vol_step_str));
    text_size.x = text_size.x * 1.5;
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));

    auto st_size = FromDIP(wxSize(text_size.x, -1));
    auto ti_size = FromDIP(wxSize(90, -1));
    // start vol
    auto start_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_vol_text = new wxStaticText(this, wxID_ANY, start_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, std::to_string(40), _L("mm/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_vol_sizer->Add(start_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    start_vol_sizer->Add(m_tiStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(start_vol_sizer);

    // end vol
    auto end_vol_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_vol_text = new wxStaticText(this, wxID_ANY, end_vol_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, std::to_string(200), _L("mm/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_vol_sizer->Add(end_vol_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    end_vol_sizer->Add(m_tiEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(end_vol_sizer);

    // vol step
    auto vol_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto vol_step_text = new wxStaticText(this, wxID_ANY, vol_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(10), _L("mm/s"), "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    vol_step_sizer->Add(vol_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    vol_step_sizer->Add(m_tiStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(vol_step_sizer);

    v_sizer->Add(settings_sizer);
    v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
		std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
		std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));

    m_btnStart->SetBackgroundColor(btn_bg_green);
    m_btnStart->SetBorderColor(wxColour(0, 150, 136));
    m_btnStart->SetTextColor(wxColour("#FFFFFE"));
    m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetCornerRadius(FromDIP(3));
    m_btnStart->Bind(wxEVT_BUTTON, &VFA_Test_Dlg::on_start, this);
    v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    m_btnStart->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(VFA_Test_Dlg::on_start), NULL, this);

    // wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
}

VFA_Test_Dlg::~VFA_Test_Dlg()
{
    // Disconnect Events
    m_btnStart->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(VFA_Test_Dlg::on_start), NULL, this);
}

void VFA_Test_Dlg::on_start(wxCommandEvent& event)
{
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!read_double || m_params.start <= 10 || m_params.step <= 0 || m_params.end < (m_params.start + m_params.step)) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 10 step >= 0\nend >= start + step)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_params.mode = CalibMode::Calib_VFA_Tower;
    m_plater->calib_VFA(m_params);
    EndModal(wxID_OK);
}

void VFA_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect)
{
    this->Refresh();
    Fit();
}



// Retraction_Test_Dlg
//

Retraction_Test_Dlg::Retraction_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater)
    : DPIDialog(parent, id, _L("Retraction test"), wxDefaultPosition, parent->FromDIP(wxSize(-1, 280)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), m_plater(plater)
{
    wxBoxSizer* v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);

    // Settings
    //
    wxString start_length_str = _L("Start retraction length: ");
    wxString end_length_str = _L("End retraction length: ");
    wxString length_step_str = _L("step: ");
    auto text_size = wxWindow::GetTextExtent(start_length_str);
    text_size.IncTo(wxWindow::GetTextExtent(end_length_str));
    text_size.IncTo(wxWindow::GetTextExtent(length_step_str));
    text_size.x = text_size.x * 1.5;
    wxStaticBoxSizer* settings_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));

    auto st_size = FromDIP(wxSize(text_size.x, -1));
    auto ti_size = FromDIP(wxSize(90, -1));
    // start length
    auto start_length_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto start_length_text = new wxStaticText(this, wxID_ANY, start_length_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStart = new TextInput(this, std::to_string(0), "mm", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));

    start_length_sizer->Add(start_length_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    start_length_sizer->Add(m_tiStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(start_length_sizer);

    // end length
    auto end_length_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto end_length_text = new wxStaticText(this, wxID_ANY, end_length_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiEnd = new TextInput(this, std::to_string(2), "mm", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    end_length_sizer->Add(end_length_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    end_length_sizer->Add(m_tiEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(end_length_sizer);

    // length step
    auto length_step_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto length_step_text = new wxStaticText(this, wxID_ANY, length_step_str, wxDefaultPosition, st_size, wxALIGN_LEFT);
    m_tiStep = new TextInput(this, wxString::FromDouble(0.1), "mm/mm", "", wxDefaultPosition, ti_size, wxTE_CENTRE);
    m_tiStart->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    length_step_sizer->Add(length_step_text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    length_step_sizer->Add(m_tiStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    settings_sizer->Add(length_step_sizer);

    v_sizer->Add(settings_sizer);
    v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
		std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
		std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));

    m_btnStart->SetBackgroundColor(btn_bg_green);
    m_btnStart->SetBorderColor(wxColour(0, 150, 136));
    m_btnStart->SetTextColor(wxColour("#FFFFFE"));
    m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetCornerRadius(FromDIP(3));
    m_btnStart->Bind(wxEVT_BUTTON, &Retraction_Test_Dlg::on_start, this);
    v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    m_btnStart->Connect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(Retraction_Test_Dlg::on_start), NULL, this);

    //wxGetApp().UpdateDlgDarkUI(this);

    Layout();
    Fit();
}

Retraction_Test_Dlg::~Retraction_Test_Dlg() {
    // Disconnect Events
    m_btnStart->Disconnect(wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler(Retraction_Test_Dlg::on_start), NULL, this);
}

void Retraction_Test_Dlg::on_start(wxCommandEvent& event) {
    bool read_double = false;
    read_double = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&m_params.start);
    read_double = read_double && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&m_params.end);
    read_double = read_double && m_tiStep->GetTextCtrl()->GetValue().ToDouble(&m_params.step);

    if (!read_double || m_params.start < 0 || m_params.step <= 0 || m_params.end < (m_params.start + m_params.step)) {
        MessageDialog msg_dlg(nullptr, _L("Please input valid values:\nstart > 0 step >= 0\nend >= start + step)"), wxEmptyString, wxICON_WARNING | wxOK);
        msg_dlg.ShowModal();
        return;
    }

    m_params.mode = CalibMode::Calib_Retraction_tower;
    m_plater->calib_retraction(m_params);
    EndModal(wxID_OK);

}

void Retraction_Test_Dlg::on_dpi_changed(const wxRect& suggested_rect) {
    this->Refresh();
    Fit();

}



// Motion_Test_Dlg (MiniMe): OrcaSlicer's cornering and input shaping towers
//

Motion_Test_Dlg::Motion_Test_Dlg(wxWindow *parent, wxWindowID id, Plater *plater, CalibMode mode)
    : DPIDialog(parent, id,
                mode == CalibMode::Calib_Cornering           ? _L("Cornering test") :
                mode == CalibMode::Calib_Input_Shaping_Freq ? _L("Input shaping frequency test") :
                                                               _L("Input shaping damping test"),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_mode(mode)
    , m_plater(plater)
{
    const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (const auto *fl = printer.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"))
        m_flavor = fl->value;
    const bool marlin = m_flavor == gcfMarlinFirmware || m_flavor == gcfMarlinLegacy;

    wxBoxSizer *v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    wxStaticBoxSizer *settings = new wxStaticBoxSizer(wxVERTICAL, this, _L("Settings"));
    const wxSize st_size = FromDIP(wxSize(190, -1));
    const wxSize ti_size = FromDIP(wxSize(100, -1));
    auto add_row = [&](const wxString &label, const wxString &unit, TextInput *&input) {
        auto row  = new wxBoxSizer(wxHORIZONTAL);
        auto text = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, st_size, wxALIGN_LEFT);
        input     = new TextInput(this, "", unit, "", wxDefaultPosition, ti_size, wxTE_CENTRE);
        input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        row->Add(text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
        row->Add(input, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
        settings->Add(row);
    };

    if (m_mode == CalibMode::Calib_Cornering) {
        if (marlin) {
            m_junction = new wxCheckBox(this, wxID_ANY, _L("Junction deviation (Marlin 2 with junction deviation on)"));
            m_junction->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { set_defaults(); });
            settings->Add(m_junction, 0, wxALL, 4);
        }
        add_row(_L("Start (bottom):"), "", m_tiStart);
        add_row(_L("End (top):"), "", m_tiEnd);
    } else {
        // shaper types the firmware has; Marlin has ZV only
        if (!marlin) {
            auto row  = new wxBoxSizer(wxHORIZONTAL);
            auto text = new wxStaticText(this, wxID_ANY, _L("Shaper type:"), wxDefaultPosition, st_size, wxALIGN_LEFT);
            wxArrayString types;
            types.Add(_L("Firmware setting"));
            if (m_flavor == gcfKlipper)
                for (const char *t : {"zv", "mzv", "zvd", "ei", "2hump_ei", "3hump_ei"})
                    types.Add(t);
            else
                for (const char *t : {"zvd", "mzv", "zvdd", "zvddd", "ei2", "ei3"})
                    types.Add(t);
            m_shaper = new wxChoice(this, wxID_ANY, wxDefaultPosition, ti_size, types);
            m_shaper->SetSelection(0);
            row->Add(text, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
            row->Add(m_shaper, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
            settings->Add(row);
        }
        if (m_mode == CalibMode::Calib_Input_Shaping_Freq) {
            add_row(_L("Start frequency (bottom):"), "Hz", m_tiStart);
            add_row(_L("End frequency (top):"), "Hz", m_tiEnd);
            add_row(_L("Damping (0 = firmware setting):"), "", m_tiDamping);
        } else {
            add_row(_L("Frequency X:"), "Hz", m_tiFreqX);
            add_row(_L("Frequency Y:"), "Hz", m_tiFreqY);
            add_row(_L("Start damping (bottom):"), "", m_tiStart);
            add_row(_L("End damping (top):"), "", m_tiEnd);
        }
    }
    v_sizer->Add(settings, 0, wxEXPAND | wxALL, FromDIP(5));

    m_hint = new wxStaticText(this, wxID_ANY, "");
    m_hint->SetForegroundColour(wxColour(144, 144, 144));
    v_sizer->Add(m_hint, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(8));
    set_defaults();

    v_sizer->Add(0, FromDIP(10), 0, wxEXPAND, 5);
    m_btnStart = new Button(this, _L("OK"));
    StateColor btn_bg(std::pair<wxColour, int>(wxColour(20, 80, 184), StateColor::Pressed),
                      std::pair<wxColour, int>(wxColour(91, 157, 255), StateColor::Hovered),
                      std::pair<wxColour, int>(wxColour(30, 110, 240), StateColor::Normal));
    m_btnStart->SetBackgroundColor(btn_bg);
    m_btnStart->SetBorderColor(wxColour(30, 110, 240));
    m_btnStart->SetTextColor(wxColour("#FFFFFE"));
    m_btnStart->SetSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetMinSize(wxSize(FromDIP(48), FromDIP(24)));
    m_btnStart->SetCornerRadius(FromDIP(3));
    m_btnStart->Bind(wxEVT_BUTTON, &Motion_Test_Dlg::on_start, this);
    v_sizer->Add(m_btnStart, 0, wxALL | wxALIGN_RIGHT, FromDIP(5));

    Layout();
    Fit();
    CenterOnParent();
}

void Motion_Test_Dlg::set_defaults()
{
    wxString hint;
    if (m_mode == CalibMode::Calib_Cornering) {
        const bool jd = m_junction != nullptr && m_junction->GetValue();
        m_tiStart->GetTextCtrl()->SetValue(jd ? "0" : "1");
        m_tiEnd->GetTextCtrl()->SetValue(jd ? "0.3" : "20");
        m_tiStart->SetLabel(jd ? "mm" : "mm/s");
        m_tiEnd->SetLabel(jd ? "mm" : "mm/s");
        hint = jd ? _L("Junction deviation in mm, usually 0 to 0.3.") :
                    m_flavor == gcfKlipper ? _L("Square corner velocity in mm/s, usually 1 to 20.") : _L("Jerk in mm/s, usually 1 to 20.");
        hint += "\n" + _L("Prints at 100 mm/s and 2000 mm/s2. Find the height where the corners start to round off or bulge,\nand read the value there (the slice preview and the G-code show it per layer).");
    } else if (m_mode == CalibMode::Calib_Input_Shaping_Freq) {
        m_tiStart->GetTextCtrl()->SetValue("15");
        m_tiEnd->GetTextCtrl()->SetValue("110");
        m_tiDamping->GetTextCtrl()->SetValue("0");
        hint = _L("Prints fast and hard (200 mm/s). Find the height with the least ringing on the X walls and on the\nY walls, and read the frequency there. Use an opaque, glossy filament.");
    } else {
        m_tiFreqX->GetTextCtrl()->SetValue("");
        m_tiFreqY->GetTextCtrl()->SetValue("");
        m_tiStart->GetTextCtrl()->SetValue("0");
        m_tiEnd->GetTextCtrl()->SetValue("0.4");
        hint = _L("Use the X and Y frequencies from the frequency test. Find the height with the least ringing and\nread the damping there; 0.1 to 0.2 is usual.");
    }
    if (m_hint != nullptr) {
        m_hint->SetLabel(hint);
        Layout();
        Fit();
    }
}

void Motion_Test_Dlg::on_start(wxCommandEvent &)
{
    Calib_Params params;
    params.mode = m_mode;
    bool ok     = m_tiStart->GetTextCtrl()->GetValue().ToDouble(&params.start) && m_tiEnd->GetTextCtrl()->GetValue().ToDouble(&params.end);
    ok          = ok && params.start >= 0. && params.end > params.start;
    if (m_tiDamping != nullptr)
        ok = ok && m_tiDamping->GetTextCtrl()->GetValue().ToDouble(&params.damping) && params.damping >= 0. && params.damping < 1.;
    if (m_tiFreqX != nullptr)
        ok = ok && m_tiFreqX->GetTextCtrl()->GetValue().ToDouble(&params.freq_x) && m_tiFreqY->GetTextCtrl()->GetValue().ToDouble(&params.freq_y) &&
             params.freq_x > 0. && params.freq_y > 0.;
    if (m_mode == CalibMode::Calib_Input_Shaping_Damp)
        ok = ok && params.end < 1.;
    if (!ok) {
        MessageDialog msg(nullptr,
                          m_mode == CalibMode::Calib_Input_Shaping_Damp ? _L("Please enter valid values:\nfrequencies above 0, 0 <= start < end < 1") :
                                                                           _L("Please enter valid values:\nstart >= 0 and end > start"),
                          wxEmptyString, wxICON_WARNING | wxOK);
        msg.ShowModal();
        return;
    }
    if (m_junction != nullptr && m_junction->GetValue())
        params.option = "jd";
    if (m_shaper != nullptr && m_shaper->GetSelection() > 0)
        params.option = m_shaper->GetStringSelection().ToStdString();
    EndModal(wxID_OK);
    m_plater->calib_motion(params);
}

void Motion_Test_Dlg::on_dpi_changed(const wxRect &suggested_rect)
{
    this->Refresh();
    Fit();
}

}} // namespace Slic3r::GUI
