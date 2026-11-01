#include "FilamentLookDialog.hpp"

#include <algorithm>

#include <wx/bmpbuttn.h>
#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include <boost/log/trivial.hpp>

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

#define PREVIEW_SIZE   wxSize(FromDIP(64), FromDIP(64))
#define SWATCH_SIZE    wxSize(FromDIP(30), FromDIP(30))
#define STOP_SIZE      wxSize(FromDIP(28), FromDIP(28))
#define PAGE_SIZE      wxSize(FromDIP(520), FromDIP(330))
#define PADDING        FromDIP(16)
#define BRAND_GREEN    wxColour(0, 174, 66)
#define TEXT_GREY      wxColour("#6B6B6B")

namespace {

struct NamedColor
{
    const char *name;
    const char *hex;
};

// Shown for every filament, official or not.
const NamedColor BASIC_COLORS[] = {
    {L("White"), "#FFFFFF"},      {L("Light grey"), "#D0D0D0"}, {L("Grey"), "#8E9089"},    {L("Dark grey"), "#545454"},
    {L("Black"), "#000000"},      {L("Red"), "#C12E1F"},        {L("Dark red"), "#7A1F1F"}, {L("Orange"), "#FF6A13"},
    {L("Yellow"), "#F4EE2A"},     {L("Gold"), "#E4BD68"},       {L("Lime"), "#BECF00"},    {L("Green"), "#00AE42"},
    {L("Dark green"), "#164B35"}, {L("Mint"), "#A6E3C4"},       {L("Cyan"), "#0086D6"},    {L("Light blue"), "#8BD5EE"},
    {L("Blue"), "#0A2989"},       {L("Purple"), "#5E43B7"},     {L("Magenta"), "#EC008C"}, {L("Pink"), "#F55A74"},
    {L("Brown"), "#9D432C"},      {L("Beige"), "#F7E6DE"},      {L("Silver"), "#A6A9AA"},  {L("Bronze"), "#847D48"},
};

// Gradient filaments that change colour along the spool.
const std::vector<std::pair<const char *, std::vector<const char *>>> BASIC_GRADIENTS = {
    {L("Sunset"), {"#FF6A13", "#EC008C", "#5E43B7"}},   {L("Ocean"), {"#8BD5EE", "#0086D6", "#0A2989"}},
    {L("Forest"), {"#BECF00", "#00AE42", "#164B35"}},   {L("Fire"), {"#F4EE2A", "#FF6A13", "#C12E1F"}},
    {L("Candy"), {"#F55A74", "#8BD5EE"}},               {L("Rainbow"), {"#C12E1F", "#F4EE2A", "#00AE42", "#0A2989"}},
    {L("Black to white"), {"#000000", "#FFFFFF"}},      {L("Aurora"), {"#00AE42", "#0086D6", "#5E43B7"}},
};

// Dual / tri-colour coextruded filaments (different colour on each side).
const std::vector<std::pair<const char *, std::vector<const char *>>> BASIC_MULTI = {
    {L("Gold / Silver"), {"#E4BD68", "#A6A9AA"}},       {L("Red / Blue"), {"#C12E1F", "#0A2989"}},
    {L("Black / White"), {"#000000", "#FFFFFF"}},       {L("Purple / Green"), {"#5E43B7", "#00AE42"}},
    {L("Pink / Blue"), {"#F55A74", "#0086D6"}},         {L("Gold / Copper / Silver"), {"#E4BD68", "#B87333", "#A6A9AA"}},
    {L("Red / Yellow / Blue"), {"#C12E1F", "#F4EE2A", "#0A2989"}},
};

// Translucent tints, 50 % opaque.
const NamedColor BASIC_TRANSLUCENT[] = {
    {L("Clear"), "#FFFFFF33"},  {L("Smoke"), "#54545480"},  {L("Red"), "#C12E1F80"},  {L("Orange"), "#FF6A1380"},
    {L("Yellow"), "#F4EE2A80"}, {L("Green"), "#00AE4280"},  {L("Cyan"), "#0086D680"}, {L("Blue"), "#0A298980"},
    {L("Purple"), "#5E43B780"}, {L("Pink"), "#F55A7480"},
};

// Typical glow-in-the-dark pigments.
const NamedColor GLOW_COLORS[] = {
    {L("Green glow"), "#7CFF4F"},  {L("Aqua glow"), "#4FFFE0"},   {L("Blue glow"), "#4FA8FF"},   {L("Yellow glow"), "#F5FF4F"},
    {L("Orange glow"), "#FFA64F"}, {L("Pink glow"), "#FF6FD8"},   {L("Purple glow"), "#B26FFF"}, {L("White glow"), "#E8F7FF"},
};

FilamentColor make_color(const std::vector<wxColour> &colors, FilamentColor::ColorType type)
{
    FilamentColor c;
    for (const wxColour &clr : colors)
        c.AddColor(clr);
    c.EndSet(type == FilamentColor::ColorType::GRADIENT_CLR ? 0 : 1);
    return c;
}

FilamentColor make_color(const std::vector<const char *> &hexes, FilamentColor::ColorType type)
{
    std::vector<wxColour> colors;
    for (const char *hex : hexes)
        colors.emplace_back(hex);
    return make_color(colors, type);
}

bool all_opaque(const FilamentColor &c)
{
    return std::all_of(c.GetColors().begin(), c.GetColors().end(), [](const wxColour &clr) { return clr.Alpha() == wxALPHA_OPAQUE; });
}

wxColour with_alpha(const wxColour &c, unsigned char alpha) { return wxColour(c.Red(), c.Green(), c.Blue(), alpha); }

} // namespace

FilamentLookDialog::FilamentLookDialog(wxWindow *parent, const wxString &fila_id, const FilamentColor &current, const wxColour &glow, const std::string &fila_type)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Filament look"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
    , m_color(current)
    , m_glow(glow)
{
    SetBackgroundColour(*wxWHITE);

    if (m_color.ColorCount() == 0)
        m_color = make_color(std::vector<wxColour>{wxColour("#00AE42")}, FilamentColor::ColorType::SINGLE_CLR);

    m_query = new FilamentColorCodeQuery();
    if (!fila_id.IsEmpty()) {
        m_official = m_query->GetFilaInfoMap(fila_id);
        m_cur_name = m_query->GetFilaColorName(fila_id, m_color);
    }

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    // Header: preview + what is selected
    wxBoxSizer *header = new wxBoxSizer(wxHORIZONTAL);
    m_preview = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap, wxDefaultPosition, PREVIEW_SIZE + wxSize(FromDIP(12), FromDIP(12)));
    header->Add(m_preview, 0, wxALIGN_CENTER_VERTICAL);
    wxBoxSizer *labels = new wxBoxSizer(wxVERTICAL);
    m_title = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_title->SetFont(Label::Head_14);
    m_subtitle = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_subtitle->SetForegroundColour(TEXT_GREY);
    wxStaticText *type_label = new wxStaticText(this, wxID_ANY, from_u8(fila_type));
    type_label->SetForegroundColour(TEXT_GREY);
    labels->Add(m_title, 0);
    labels->AddSpacer(FromDIP(4));
    labels->Add(m_subtitle, 0);
    labels->AddSpacer(FromDIP(2));
    labels->Add(type_label, 0);
    header->Add(labels, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    main_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    // Filter
    m_filter = new ::TextInput(this, wxEmptyString, wxEmptyString, "search", wxDefaultPosition, wxSize(-1, FromDIP(28)));
    m_filter->GetTextCtrl()->SetHint(_L("Filter colours by name or code, e.g. blue or 10900"));
    m_filter->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { apply_filter(m_filter->GetTextCtrl()->GetValue()); });
    main_sizer->Add(m_filter, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    // Tabs
    const wxString tab_names[pgCount] = {_L("Solid"), _L("Gradient"), _L("Multi-color"), _L("Translucent"), _L("Glow")};
    wxBoxSizer *tabs = new wxBoxSizer(wxHORIZONTAL);
    for (int i = 0; i < pgCount; ++i) {
        Button *tab = new Button(this, tab_names[i]);
        tab->SetFont(Label::Body_13);
        tab->SetCornerRadius(FromDIP(14));
        tab->SetMinSize(wxSize(-1, FromDIP(28)));
        tab->SetPaddingSize(wxSize(FromDIP(14), FromDIP(4)));
        tab->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { select_page(Page(i)); });
        tabs->Add(tab, 0, wxRIGHT, FromDIP(6));
        m_tab_buttons.push_back(tab);
    }
    main_sizer->Add(tabs, 0, wxLEFT | wxRIGHT | wxTOP, PADDING);

    // Pages
    m_book = new wxSimplebook(this, wxID_ANY);
    m_book->SetBackgroundColour(*wxWHITE);
    m_no_match.resize(pgCount, nullptr);
    for (int i = 0; i < pgCount; ++i) {
        wxScrolledWindow *page = create_page(Page(i));
        m_book->AddPage(page, tab_names[i]);
        m_pages.push_back(page);
    }
    main_sizer->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PADDING);

    // OK / Cancel
    StateColor btn_bg_green(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                            std::pair<wxColour, int>(BRAND_GREEN, StateColor::Normal));
    StateColor btn_bg_white(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Pressed),
                            std::pair<wxColour, int>(wxColour(238, 238, 238), StateColor::Hovered),
                            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    m_cancel_btn = new Button(this, _L("Cancel"));
    m_cancel_btn->SetBackgroundColor(btn_bg_white);
    m_cancel_btn->SetBorderColor(wxColour(38, 46, 48));
    m_cancel_btn->SetFont(Label::Body_12);
    m_cancel_btn->SetMinSize(wxSize(FromDIP(64), FromDIP(26)));
    m_cancel_btn->SetCornerRadius(FromDIP(13));
    m_cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    buttons->Add(m_cancel_btn, 0, wxRIGHT, FromDIP(10));
    m_ok_btn = new Button(this, _L("OK"));
    m_ok_btn->SetBackgroundColor(btn_bg_green);
    m_ok_btn->SetBorderColor(BRAND_GREEN);
    m_ok_btn->SetTextColor(wxColour("#FFFFFE"));
    m_ok_btn->SetFont(Label::Body_12);
    m_ok_btn->SetMinSize(wxSize(FromDIP(64), FromDIP(26)));
    m_ok_btn->SetCornerRadius(FromDIP(13));
    m_ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    buttons->Add(m_ok_btn, 0);
    main_sizer->Add(buttons, 0, wxEXPAND | wxALL, PADDING);

    SetSizer(main_sizer);

    // Open on the tab that matches the current look.
    Page start = pgSolid;
    if (m_glow.IsOk())
        start = pgGlow;
    else if (m_color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR)
        start = pgGradient;
    else if (m_color.m_color_type == FilamentColor::ColorType::MULTI_CLR)
        start = pgMulti;
    else if (!all_opaque(m_color))
        start = pgTranslucent;
    select_page(start);
    update_preview();
    update_opacity_controls();

    Layout();
    main_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

FilamentLookDialog::~FilamentLookDialog()
{
    delete m_query;
    m_query = nullptr;
}

wxScrolledWindow *FilamentLookDialog::create_page(Page page)
{
    wxScrolledWindow *win = new wxScrolledWindow(m_book, wxID_ANY, wxDefaultPosition, PAGE_SIZE, wxVSCROLL);
    win->SetBackgroundColour(*wxWHITE);
    win->SetScrollRate(0, FromDIP(10));
    // Keep the vertical scrollbar visible: with a wrap sizer, a scrollbar that appears and disappears
    // changes the width, re-wraps the swatches and can make the layout loop forever (UI freeze).
    win->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_ALWAYS);
    win->SetMinSize(PAGE_SIZE);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

    switch (page) {
    case pgSolid: {
        add_official_swatches(win, sizer, page);
        wxBoxSizer *grid = add_section(win, sizer, _L("Basic colours"));
        for (const NamedColor &c : BASIC_COLORS)
            add_swatch(win, grid, make_color(std::vector<const char *>{c.hex}, FilamentColor::ColorType::SINGLE_CLR), _(c.name));
        add_section(win, sizer, _L("Custom"), _L("Any colour. Set transparency on the Translucent tab."));
        Button *custom = new Button(win, _L("Choose a colour") + "...");
        custom->SetFont(Label::Body_12);
        custom->SetCornerRadius(FromDIP(12));
        custom->SetMinSize(wxSize(-1, FromDIP(26)));
        custom->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const wxColour picked = pick_colour(with_alpha(m_color.GetColors().front(), wxALPHA_OPAQUE));
            if (picked.IsOk())
                set_color(make_color(std::vector<wxColour>{picked}, FilamentColor::ColorType::SINGLE_CLR));
        });
        sizer->Add(custom, 0, wxLEFT | wxTOP, FromDIP(4));
        break;
    }
    case pgGradient: {
        add_official_swatches(win, sizer, page);
        wxBoxSizer *grid = add_section(win, sizer, _L("Gradients"), _L("The colour changes slowly along the spool. Shown bottom to top on the model."));
        for (const auto &g : BASIC_GRADIENTS)
            add_swatch(win, grid, make_color(g.second, FilamentColor::ColorType::GRADIENT_CLR), _(g.first));
        add_stop_editor(win, sizer, page);
        break;
    }
    case pgMulti: {
        add_official_swatches(win, sizer, page);
        wxBoxSizer *grid = add_section(win, sizer, _L("Dual and tri-color"), _L("Each side of the extrusion has a different colour. Shown as sections around the model."));
        for (const auto &m : BASIC_MULTI)
            add_swatch(win, grid, make_color(m.second, FilamentColor::ColorType::MULTI_CLR), _(m.first));
        add_stop_editor(win, sizer, page);
        break;
    }
    case pgTranslucent: {
        add_section(win, sizer, _L("Opacity"), _L("How see-through the filament looks. Below 10% it is shown as frosted white."));
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        m_opacity_slider = new wxSlider(win, wxID_ANY, 100, 0, 100, wxDefaultPosition, wxSize(FromDIP(260), -1));
        m_opacity_label = new wxStaticText(win, wxID_ANY, "100%", wxDefaultPosition, wxSize(FromDIP(48), -1));
        m_opacity_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            const unsigned char alpha = (unsigned char) std::lround(m_opacity_slider->GetValue() * 255.0 / 100.0);
            std::vector<wxColour> colors;
            for (const wxColour &c : m_color.GetColors())
                colors.push_back(with_alpha(c, alpha));
            set_color(make_color(colors, m_color.m_color_type));
        });
        row->Add(m_opacity_slider, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(m_opacity_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        sizer->Add(row, 0, wxLEFT, FromDIP(4));
        add_official_swatches(win, sizer, page);
        wxBoxSizer *grid = add_section(win, sizer, _L("Translucent tints"));
        for (const NamedColor &c : BASIC_TRANSLUCENT)
            add_swatch(win, grid, make_color(std::vector<const char *>{c.hex}, FilamentColor::ColorType::SINGLE_CLR), _(c.name));
        break;
    }
    case pgGlow: {
        wxBoxSizer *grid = add_section(win, sizer, _L("Glow colour"),
                                       _L("The colour a glow-in-the-dark filament glows. The filament's own colour stays as chosen in the other tabs."));
        for (const NamedColor &c : GLOW_COLORS)
            add_glow_swatch(win, grid, wxColour(c.hex), _(c.name));
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        Button *custom = new Button(win, _L("Custom glow") + "...");
        custom->SetFont(Label::Body_12);
        custom->SetCornerRadius(FromDIP(12));
        custom->SetMinSize(wxSize(-1, FromDIP(26)));
        custom->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const wxColour picked = pick_colour(m_glow.IsOk() ? m_glow : wxColour("#7CFF4F"));
            if (picked.IsOk()) {
                m_glow = with_alpha(picked, wxALPHA_OPAQUE);
                select_swatch(size_t(-1));
                update_preview();
            }
        });
        Button *none = new Button(win, _L("No glow"));
        none->SetFont(Label::Body_12);
        none->SetCornerRadius(FromDIP(12));
        none->SetMinSize(wxSize(-1, FromDIP(26)));
        none->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_glow = wxNullColour;
            select_swatch(size_t(-1));
            update_preview();
        });
        row->Add(custom, 0, wxRIGHT, FromDIP(8));
        row->Add(none, 0);
        sizer->Add(row, 0, wxLEFT | wxTOP, FromDIP(4));
        break;
    }
    default: break;
    }

    m_no_match[page] = new wxStaticText(win, wxID_ANY, _L("No colours match the filter."));
    m_no_match[page]->SetForegroundColour(TEXT_GREY);
    m_no_match[page]->Hide();
    sizer->Add(m_no_match[page], 0, wxALL, FromDIP(8));

    win->SetSizer(sizer);
    win->FitInside();
    return win;
}

wxBoxSizer *FilamentLookDialog::add_section(wxWindow *page, wxBoxSizer *page_sizer, const wxString &title, const wxString &hint)
{
    wxStaticText *label = new wxStaticText(page, wxID_ANY, title);
    label->SetFont(Label::Head_13);
    page_sizer->Add(label, 0, wxLEFT | wxTOP, FromDIP(4));
    if (!hint.IsEmpty()) {
        wxStaticText *hint_label = new wxStaticText(page, wxID_ANY, hint);
        hint_label->SetForegroundColour(TEXT_GREY);
        hint_label->Wrap(PAGE_SIZE.GetWidth() - FromDIP(30));
        page_sizer->Add(hint_label, 0, wxLEFT | wxTOP, FromDIP(4));
    }
    return add_grid(page, page_sizer);
}

wxBoxSizer *FilamentLookDialog::add_grid(wxWindow *page, wxBoxSizer *page_sizer)
{
    wxWrapSizer *grid = new wxWrapSizer(wxHORIZONTAL);
    page_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(4));
    return grid;
}

void FilamentLookDialog::add_swatch(wxWindow *page, wxBoxSizer *grid, const FilamentColor &color, const wxString &name, const wxString &code)
{
    Swatch sw;
    sw.color = color;
    sw.name  = name;
    sw.code  = code;
    sw.grid  = grid;
    const bool selected = m_selected < 0 && !m_glow.IsOk() && color.MatchesColorSet(m_color);
    sw.button = new wxBitmapButton(page, wxID_ANY, swatch_bitmap(color, wxNullColour, SWATCH_SIZE, selected), wxDefaultPosition,
                                   SWATCH_SIZE + wxSize(FromDIP(4), FromDIP(4)), wxBU_EXACTFIT | wxNO_BORDER);
    sw.button->SetBackgroundColour(*wxWHITE);
    sw.button->SetToolTip(code.IsEmpty() ? name : wxString::Format("%s (%s)", name, code));
    const size_t idx = m_swatches.size();
    sw.button->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) { select_swatch(idx); });
    grid->Add(sw.button, 0, wxALL, FromDIP(2));
    if (selected)
        m_selected = int(idx);
    m_swatches.push_back(sw);
}

void FilamentLookDialog::add_glow_swatch(wxWindow *page, wxBoxSizer *grid, const wxColour &glow, const wxString &name)
{
    Swatch sw;
    sw.glow    = glow;
    sw.is_glow = true;
    sw.name    = name;
    sw.grid    = grid;
    sw.button  = new wxBitmapButton(page, wxID_ANY, swatch_bitmap(m_color, glow, SWATCH_SIZE, m_glow.IsOk() && m_glow == glow), wxDefaultPosition,
                                    SWATCH_SIZE + wxSize(FromDIP(4), FromDIP(4)), wxBU_EXACTFIT | wxNO_BORDER);
    sw.button->SetBackgroundColour(*wxWHITE);
    sw.button->SetToolTip(name);
    const size_t idx = m_swatches.size();
    sw.button->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) { select_swatch(idx); });
    grid->Add(sw.button, 0, wxALL, FromDIP(2));
    m_swatches.push_back(sw);
}

void FilamentLookDialog::add_official_swatches(wxWindow *page, wxBoxSizer *page_sizer, Page page_kind)
{
    if (m_official == nullptr || m_official->GetFilamentColor2CodeMap() == nullptr)
        return;
    wxBoxSizer *grid = nullptr;
    for (const auto &[color, code] : *m_official->GetFilamentColor2CodeMap()) {
        if (code == nullptr)
            continue;
        bool fits = false;
        switch (page_kind) {
        case pgSolid: fits = color.m_color_type == FilamentColor::ColorType::SINGLE_CLR && all_opaque(color); break;
        case pgTranslucent: fits = color.m_color_type == FilamentColor::ColorType::SINGLE_CLR && !all_opaque(color); break;
        case pgGradient: fits = color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR; break;
        case pgMulti: fits = color.m_color_type == FilamentColor::ColorType::MULTI_CLR; break;
        default: break;
        }
        if (!fits)
            continue;
        if (grid == nullptr)
            grid = add_section(page, page_sizer, wxString::Format(_L("Official %s colours"), m_official->GetFilaType()));
        add_swatch(page, grid, code->GetFilaColor(), code->GetFilaColorName(), code->GetFilaColorCode());
    }
}

void FilamentLookDialog::add_stop_editor(wxWindow *page, wxBoxSizer *page_sizer, Page page_kind)
{
    wxStaticText *label = new wxStaticText(page, wxID_ANY, _L("Make your own"));
    label->SetFont(Label::Head_13);
    page_sizer->Add(label, 0, wxLEFT | wxTOP, FromDIP(4));
    wxStaticText *hint = new wxStaticText(page, wxID_ANY, _L("Click a colour to change it. Use + and - to add or remove colours (2 to 4)."));
    hint->SetForegroundColour(TEXT_GREY);
    page_sizer->Add(hint, 0, wxLEFT | wxTOP, FromDIP(4));
    m_stop_rows[page_kind]    = new wxBoxSizer(wxHORIZONTAL);
    m_stop_parents[page_kind] = page;
    page_sizer->Add(m_stop_rows[page_kind], 0, wxALL, FromDIP(4));
    if (m_stops.empty()) {
        m_stops = m_color.GetColors();
        if (m_stops.size() < 2)
            m_stops.push_back(*wxWHITE);
    }
    rebuild_stop_editors();
}

void FilamentLookDialog::rebuild_stop_editors()
{
    for (int p : {pgGradient, pgMulti}) {
        wxBoxSizer *row = m_stop_rows[p];
        wxWindow *page  = m_stop_parents[p];
        if (row == nullptr || page == nullptr)
            continue;
        row->Clear(true);
        const auto type = p == pgGradient ? FilamentColor::ColorType::GRADIENT_CLR : FilamentColor::ColorType::MULTI_CLR;
        for (size_t i = 0; i < m_stops.size(); ++i) {
            wxBitmapButton *stop = new wxBitmapButton(page, wxID_ANY,
                                                      swatch_bitmap(make_color(std::vector<wxColour>{m_stops[i]}, FilamentColor::ColorType::SINGLE_CLR), wxNullColour, STOP_SIZE, false),
                                                      wxDefaultPosition, STOP_SIZE + wxSize(FromDIP(4), FromDIP(4)), wxBU_EXACTFIT | wxNO_BORDER);
            stop->SetBackgroundColour(*wxWHITE);
            stop->SetToolTip(m_stops[i].GetAsString(wxC2S_HTML_SYNTAX));
            stop->Bind(wxEVT_BUTTON, [this, i, type](wxCommandEvent &) {
                const wxColour picked = pick_colour(m_stops[i]);
                if (!picked.IsOk())
                    return;
                m_stops[i] = picked;
                set_color(make_color(m_stops, type));
                CallAfter([this]() { rebuild_stop_editors(); });
            });
            row->Add(stop, 0, wxRIGHT, FromDIP(4));
        }
        Button *add = new Button(page, "+");
        add->SetCornerRadius(FromDIP(12));
        add->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
        add->Enable(m_stops.size() < 4);
        add->Bind(wxEVT_BUTTON, [this, type](wxCommandEvent &) {
            if (m_stops.size() >= 4)
                return;
            m_stops.push_back(m_stops.back());
            set_color(make_color(m_stops, type));
            CallAfter([this]() { rebuild_stop_editors(); });
        });
        Button *remove = new Button(page, "-");
        remove->SetCornerRadius(FromDIP(12));
        remove->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
        remove->Enable(m_stops.size() > 2);
        remove->Bind(wxEVT_BUTTON, [this, type](wxCommandEvent &) {
            if (m_stops.size() <= 2)
                return;
            m_stops.pop_back();
            set_color(make_color(m_stops, type));
            CallAfter([this]() { rebuild_stop_editors(); });
        });
        row->Add(add, 0, wxLEFT, FromDIP(6));
        row->Add(remove, 0, wxLEFT, FromDIP(4));
        page->Layout();
        static_cast<wxScrolledWindow *>(page)->FitInside();
    }
}

void FilamentLookDialog::select_page(Page page)
{
    StateColor selected_bg(std::pair<wxColour, int>(BRAND_GREEN, StateColor::Normal));
    StateColor normal_bg(std::pair<wxColour, int>(wxColour(225, 225, 225), StateColor::Hovered),
                         std::pair<wxColour, int>(wxColour(242, 242, 242), StateColor::Normal));
    for (int i = 0; i < (int) m_tab_buttons.size(); ++i) {
        const bool sel = i == int(page);
        m_tab_buttons[i]->SetBackgroundColor(sel ? selected_bg : normal_bg);
        m_tab_buttons[i]->SetBorderColor(sel ? BRAND_GREEN : wxColour(242, 242, 242));
        m_tab_buttons[i]->SetTextColor(sel ? wxColour("#FFFFFE") : wxColour(38, 46, 48));
        m_tab_buttons[i]->Refresh();
    }
    m_book->SetSelection(size_t(page));
}

void FilamentLookDialog::select_swatch(size_t idx)
{
    const bool picking_glow = idx < m_swatches.size() && m_swatches[idx].is_glow;
    if (idx < m_swatches.size()) {
        if (picking_glow) {
            m_glow = m_swatches[idx].glow;
        } else {
            set_color(m_swatches[idx].color);
            m_stops = m_color.GetColors();
            if (m_stops.size() < 2)
                m_stops.push_back(*wxWHITE);
            CallAfter([this]() { rebuild_stop_editors(); });
        }
    }
    // Redraw selection marks: one colour swatch and one glow swatch can be selected at once.
    for (size_t i = 0; i < m_swatches.size(); ++i) {
        Swatch &sw = m_swatches[i];
        bool sel   = sw.is_glow ? (m_glow.IsOk() && sw.glow == m_glow) : (i == idx || (!picking_glow && int(i) == m_selected && idx >= m_swatches.size()));
        if (!sw.is_glow && picking_glow)
            sel = int(i) == m_selected;
        sw.button->SetBitmap(swatch_bitmap(sw.is_glow ? m_color : sw.color, sw.is_glow ? sw.glow : wxNullColour, SWATCH_SIZE, sel));
    }
    if (idx < m_swatches.size() && !picking_glow)
        m_selected = int(idx);
    else if (idx >= m_swatches.size() && !picking_glow && m_selected >= 0 && !m_swatches[m_selected].color.MatchesColorSet(m_color))
        m_selected = -1;
    update_preview();
}

void FilamentLookDialog::apply_filter(const wxString &text)
{
    const wxString needle = text.Lower().Trim().Trim(false);
    std::vector<bool> page_has_match(pgCount, false);
    for (Swatch &sw : m_swatches) {
        const bool match = needle.IsEmpty() || sw.name.Lower().Contains(needle) || sw.code.Lower().Contains(needle);
        sw.button->Show(match);
        if (match) {
            for (int p = 0; p < pgCount; ++p)
                if (sw.button->GetParent() == m_pages[p])
                    page_has_match[p] = true;
        }
    }
    for (int p = 0; p < pgCount; ++p) {
        m_no_match[p]->Show(!needle.IsEmpty() && !page_has_match[p]);
        m_pages[p]->Layout();
        m_pages[p]->FitInside();
    }
}

void FilamentLookDialog::set_color(const FilamentColor &color)
{
    m_color = color;
    m_cur_name.clear();
    // Glow swatches show the filament colour behind the glow, so refresh them.
    for (Swatch &sw : m_swatches)
        if (sw.is_glow)
            sw.button->SetBitmap(swatch_bitmap(m_color, sw.glow, SWATCH_SIZE, m_glow.IsOk() && sw.glow == m_glow));
    update_preview();
    update_opacity_controls();
}

void FilamentLookDialog::update_opacity_controls()
{
    if (m_opacity_slider == nullptr || m_color.ColorCount() == 0)
        return;
    const int percent = int(std::lround(m_color.GetColors().front().Alpha() * 100.0 / 255.0));
    m_opacity_slider->SetValue(percent);
    m_opacity_label->SetLabel(wxString::Format("%d%%", percent));
}

void FilamentLookDialog::update_preview()
{
    m_preview->SetBitmap(swatch_bitmap(m_color, m_glow, PREVIEW_SIZE + wxSize(FromDIP(12), FromDIP(12)), false));
    if (m_selected >= 0 && m_selected < (int) m_swatches.size() && m_swatches[m_selected].color.MatchesColorSet(m_color))
        m_title->SetLabel(m_swatches[m_selected].name);
    else if (!m_cur_name.IsEmpty())
        m_title->SetLabel(m_cur_name);
    else if (m_color.ColorCount() == 1)
        m_title->SetLabel(m_color.GetColors().front().GetAsString(wxC2S_HTML_SYNTAX));
    else
        m_title->SetLabel(_L("Custom"));
    m_subtitle->SetLabel(describe());
    Layout();
}

wxString FilamentLookDialog::describe() const
{
    const wxString SEP = wxString::FromUTF8(" \xC2\xB7 "); // middle dot, independent of the Windows code page
    wxString text;
    switch (m_color.m_color_type) {
    case FilamentColor::ColorType::GRADIENT_CLR: text = wxString::Format(_L("Gradient, %d colours"), int(m_color.ColorCount())); break;
    case FilamentColor::ColorType::MULTI_CLR: text = wxString::Format(_L("Multi-color, %d colours"), int(m_color.ColorCount())); break;
    default: text = _L("Solid"); break;
    }
    if (!all_opaque(m_color))
        text += wxString::Format(SEP + _L("%d%% opaque"), int(std::lround(m_color.GetColors().front().Alpha() * 100.0 / 255.0)));
    if (m_glow.IsOk())
        text += SEP + _L("glows") + " " + m_glow.GetAsString(wxC2S_HTML_SYNTAX);
    return text;
}

wxBitmap FilamentLookDialog::swatch_bitmap(const FilamentColor &color, const wxColour &glow, const wxSize &size, bool selected) const
{
    wxBitmap result(size.GetWidth(), size.GetHeight());
    wxMemoryDC dc(result);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();

    const int inset = glow.IsOk() ? std::max(4, size.GetWidth() / 6) : FromDIP(2);
    if (glow.IsOk()) {
        // Soft halo: rings of the glow colour fading outwards.
        if (wxGraphicsContext *gc = wxGraphicsContext::Create(dc)) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            const int rings = inset;
            for (int r = rings; r > 0; --r) {
                const unsigned char a = (unsigned char) (40 + 150 * (rings - r) / rings);
                gc->SetBrush(wxBrush(with_alpha(glow, a)));
                gc->DrawRoundedRectangle(inset - r, inset - r, size.GetWidth() - 2 * (inset - r), size.GetHeight() - 2 * (inset - r), r + 3);
            }
            delete gc;
        }
    }
    const wxSize inner(size.GetWidth() - 2 * inset, size.GetHeight() - 2 * inset);
    wxBitmap base = create_filament_bitmap(color.GetColors(), inner, color.m_color_type == FilamentColor::ColorType::GRADIENT_CLR);
    if (base.IsOk())
        dc.DrawBitmap(base, inset, inset, true);
    if (selected) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(BRAND_GREEN, FromDIP(2)));
        dc.DrawRoundedRectangle(1, 1, size.GetWidth() - 2, size.GetHeight() - 2, FromDIP(4));
    }
    dc.SelectObject(wxNullBitmap);
    return result;
}

wxColour FilamentLookDialog::pick_colour(const wxColour &initial)
{
    wxColourData data;
    data.SetChooseFull(true);
    data.SetColour(with_alpha(initial, wxALPHA_OPAQUE));
    BOOST_LOG_TRIVIAL(warning) << "FilamentLookDialog: opening system colour picker";
    const wxColourData result = show_sys_picker_dialog(this, data);
    BOOST_LOG_TRIVIAL(warning) << "FilamentLookDialog: system colour picker closed";
    const wxColour &picked    = result.GetColour();
    if (!picked.IsOk() || picked == data.GetColour())
        return wxNullColour;
    // The system dialog has no alpha: keep the one we started with.
    return with_alpha(picked, initial.Alpha());
}

void FilamentLookDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    for (Button *b : m_tab_buttons)
        b->Rescale();
    m_ok_btn->Rescale();
    m_cancel_btn->Rescale();
    update_preview();
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
