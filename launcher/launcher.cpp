// launcher.cpp -- the launcher window.
#include "launcher.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <shellapi.h>
#endif

#include "game.h"
#include "icon.h"
#include "settings.h"
#include "version.h"

const char* const APP_TITLE = "Test Drive Enhanced";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SOURCE = "https://github.com/kylofon/testdrive-enhanced";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";
const char* const SECTION = "Game";

const int ID_GRAPHICS = wxID_HIGHEST + 1, ID_KEY_BINDINGS = wxID_HIGHEST + 2, ID_ABOUT = wxID_HIGHEST + 3;
enum Page { PAGE_MAIN, PAGE_GRAPHICS, PAGE_KEYS };

const int MIN_SCALE = 1, MAX_SCALE = 6, DEFAULT_SCALE = 3;
const int MIN_RES = 1, MAX_RES = 8;
const int MAX_FRAME_RATE = 240;

#ifdef __WXMSW__
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}
#else
// A label and a link on one line, for the portable About box.
void AddLink(wxWindow* parent, wxSizer* sizer, const wxString& label, const wxString& text, const wxString& url) {
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label + " "), 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxHyperlinkCtrl(parent, wxID_ANY, text, url), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line);
}
#endif

wxStaticText* GreyText(wxWindow* parent, const wxString& text = wxEmptyString) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    return label;
}

// A label, a text field and a Browse button in a row of a two-column grid.
wxTextCtrl* PathRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip,
                    wxStaticText** labelOut, wxButton** browse) {
    auto* text = new wxStaticText(parent, wxID_ANY, label);
    grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);
    if (labelOut) *labelOut = text;
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* field = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(parent->FromDIP(300), -1));
    field->SetToolTip(tip);
    *browse = new wxButton(parent, wxID_ANY, wxString::FromUTF8("B&rowse…"));
    row->Add(field, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(8));
    row->Add(*browse, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(row, 1, wxEXPAND);
    return field;
}

// A label and a choice in a row of a two-column grid.
wxChoice* ChoiceRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* choice = new wxChoice(parent, wxID_ANY);
    choice->SetToolTip(tip);
    grid->Add(choice, 0, wxALIGN_CENTER_VERTICAL);
    return choice;
}

Look LoadLook() {
    Look look;
    look.resScale = wxMax(MIN_RES, wxMin(MAX_RES, settings::GetInt(SECTION, "Resolution", look.resScale)));
    look.spriteMax = settings::GetInt(SECTION, "SpriteDetailMax", 1) != 0;
    look.frameRate = wxMax(0, wxMin(MAX_FRAME_RATE, settings::GetInt(SECTION, "FrameRate", look.frameRate)));
    return look;
}

void SaveLook(const Look& look) {
    settings::SetInt(SECTION, "Resolution", look.resScale);
    settings::SetInt(SECTION, "SpriteDetailMax", look.spriteMax ? 1 : 0);
    settings::SetInt(SECTION, "FrameRate", look.frameRate);
}

// A folder or file left at its default is stored empty, so it follows the launcher if the whole folder moves.
void SavePath(const char* key, const wxString& path, const wxString& def) {
    settings::SetString(SECTION, key, wxFileName(path).SameAs(wxFileName(def)) ? wxString() : path);
}

}  // namespace

LauncherFrame::LauncherFrame()
    : wxFrame(nullptr, wxID_ANY, APP_TITLE, wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)) {
    SetIcons(AppIcons());
    topmost_ = settings::GetInt("Preferences", "AlwaysOnTop", 0) != 0;
    if (topmost_) SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);
    bindings_ = keys::Load();
    edited_ = bindings_;
    look_ = LoadLook();

    // The launcher and, in its place while they are open, Graphics and Key Bindings.
    book_ = new wxSimplebook(this);
    book_->AddPage(CreateMainPage(book_), "Game");
    book_->AddPage(CreateGraphicsPage(book_), "Graphics");
    book_->AddPage(CreateKeysPage(book_), "Key Bindings");
    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(book_, 1, wxEXPAND);
    SetSizer(all);
    CreateMenus();

    // Settings from the last run.
    const wxString dir = settings::GetString(SECTION, "GameFolder", "");
    const wxString disk = settings::GetString(SECTION, "AmigaDisk", "");
    const wxString program = settings::GetString(SECTION, "Program", "");
    folder_->ChangeValue(dir.empty() ? DefaultGameDir() : dir);
    disk_->ChangeValue(disk.empty() ? DefaultAmigaDisk() : disk);
    program_->ChangeValue(program.empty() ? DefaultProgram() : program);
    (settings::GetString(SECTION, "Version", "pc") == "amiga" ? amigaVersion_ : pc_)->SetValue(true);
    scale_->SetSelection(
        wxMax(MIN_SCALE, wxMin(MAX_SCALE, settings::GetInt(SECTION, "Scale", DEFAULT_SCALE))) - MIN_SCALE);
    fullscreen_->SetValue(settings::GetInt(SECTION, "Fullscreen", 0) != 0);
    biosKeys_->SetValue(settings::GetInt(SECTION, "BiosKeys", 0) != 0);
    for (int i = 0; i < AMIGA_FEATURE_COUNT; ++i)
        features_[i]->SetValue(settings::GetInt(SECTION, AMIGA_FEATURES[i].setting, 1) != 0);
    UpdateAllBox();
    ShowLook(look_);
    ShowEditedKeys();
    loading_ = false;
    ShowVersion();

    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (event.GetActive()) UpdateState();  // files may have been copied in meanwhile
        event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        Save();
        Destroy();
    });

    FitPage();
    if (!settings::RestoreWindowPosition(SECTION, this)) Centre();
}

wxWindow* LauncherFrame::CreateMainPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    const int margin = FromDIP(12), gap = FromDIP(8), small = FromDIP(4);

    // Version
    auto* versionBox = new wxStaticBoxSizer(wxHORIZONTAL, page, "Version");
    wxWindow* vb = versionBox->GetStaticBox();
    pc_ = new wxRadioButton(vb, wxID_ANY, "P&C", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    pc_->SetToolTip("The DOS game, enhanced.");
    amigaVersion_ = new wxRadioButton(vb, wxID_ANY, "&Amiga");
    amigaVersion_->SetToolTip("The DOS game with the Amiga release's features chosen below mixed in. It needs "
                              "the Amiga disk as well as the DOS game's files.");
    versionBox->Add(pc_, 0, wxALL, gap);
    versionBox->Add(amigaVersion_, 0, wxTOP | wxBOTTOM | wxRIGHT, gap);
    pc_->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { ShowVersion(); });
    amigaVersion_->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) { ShowVersion(); });

    // Game files
    auto* filesBox = new wxStaticBoxSizer(wxVERTICAL, page, "Game files");
    wxWindow* fb = filesBox->GetStaticBox();
    auto* filesGrid = new wxFlexGridSizer(2, gap, gap);
    filesGrid->AddGrowableCol(1);
    wxButton* browseFolder = nullptr;
    wxButton* browseProgram = nullptr;
    folder_ = PathRow(fb, filesGrid, "&Folder:", "The folder with the DOS game's files (TDEGA.EXE and the rest).",
                      nullptr, &browseFolder);
    disk_ = PathRow(fb, filesGrid, "Amiga &disk:",
                    "The Amiga disk image (.adf), a folder holding it, or a folder with the files of the disk.",
                    &diskLabel_, &diskBrowse_);
    program_ = PathRow(fb, filesGrid, "Pro&gram:", "testdrive-enhanced, the game.", nullptr, &browseProgram);
    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    statusIcon_ = new wxStaticBitmap(fb, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_MENU));
    statusNote_ = new wxStaticText(fb, wxID_ANY, wxEmptyString);
    statusRow->Add(statusIcon_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, small);
    statusRow->Add(statusNote_, 1, wxALIGN_CENTER_VERTICAL);
    filesBox->Add(filesGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
    filesBox->Add(statusRow, 0, wxEXPAND | wxALL, gap);
    for (wxTextCtrl* text : {folder_, disk_, program_})
        text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (!loading_) UpdateState();
        });
    browseFolder->Bind(wxEVT_BUTTON,
                       [this](wxCommandEvent&) { Browse(folder_, true, "Choose the folder with the DOS game's files"); });
    diskBrowse_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Browse(disk_, false, "Choose the Amiga disk image"); });
    browseProgram->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Browse(program_, false, "Choose testdrive-enhanced"); });

    // Options
    auto* optionsBox = new wxStaticBoxSizer(wxVERTICAL, page, "Options");
    wxWindow* ob = optionsBox->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    scale_ = ChoiceRow(ob, grid, "&Window size:", "The window's size when the game starts. Alt+Enter switches to "
                                                 "full screen.");
    for (int s = MIN_SCALE; s <= MAX_SCALE; ++s) scale_->Append(wxString::Format(L"%d × %d", 320 * s, 240 * s));
    optionsBox->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    fullscreen_ = new wxCheckBox(ob, wxID_ANY, "Start in f&ull screen (Alt+Enter switches)");
    optionsBox->Add(fullscreen_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    biosKeys_ = new wxCheckBox(ob, wxID_ANY, "Original &keyboard handling");
    biosKeys_->SetToolTip("Driving keys act only through the keyboard's key repeat, exactly like the original. "
                          "When off, keys act for as long as they are held.");
    optionsBox->Add(biosKeys_, 0, wxALL, gap);

    // Amiga features: the box checks or clears them all; any mix can be played.
    amigaBox_ = new wxStaticBoxSizer(wxVERTICAL, page, "Amiga features");
    wxWindow* ab = amigaBox_->GetStaticBox();
    allAmiga_ = new wxCheckBox(ab, wxID_ANY, "A&ll Amiga features", wxDefaultPosition, wxDefaultSize, wxCHK_3STATE);
    allAmiga_->SetToolTip("Check or clear every Amiga feature below. Any mix of them can be played.");
    amigaBox_->Add(allAmiga_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    auto* featureGrid = new wxFlexGridSizer(2, small, FromDIP(24));
    const int half = (AMIGA_FEATURE_COUNT + 1) / 2;
    for (int i = 0; i < AMIGA_FEATURE_COUNT; ++i) features_.push_back(new wxCheckBox(ab, wxID_ANY, AMIGA_FEATURES[i].label));
    for (int row = 0; row < half; ++row)    // two columns, read down
        for (int col = 0; col < 2; ++col) {
            const int i = col * half + row;
            if (i < AMIGA_FEATURE_COUNT) featureGrid->Add(features_[i]);
            else featureGrid->AddSpacer(0);
        }
    for (int i = 0; i < AMIGA_FEATURE_COUNT; ++i) {
        features_[i]->SetToolTip(AMIGA_FEATURES[i].tip);
        features_[i]->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateAllBox(); });
    }
    amigaBox_->Add(featureGrid, 0, wxALL, gap);
    allAmiga_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        // A click on "some" checks them all, like a click on "none".
        const bool on = allAmiga_->Get3StateValue() != wxCHK_UNCHECKED;
        for (wxCheckBox* f : features_) f->SetValue(on);
        UpdateAllBox();
    });

    // Buttons
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    play_ = new wxButton(page, wxID_ANY, "&Play");
    auto* close = new wxButton(page, wxID_CLOSE, "Close");
    buttons->AddStretchSpacer();
    buttons->Add(play_, 0, wxRIGHT, gap);
    buttons->Add(close);
    play_->SetDefault();
    play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Play(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(versionBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(filesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(optionsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(amigaBox_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    page->SetSizer(all);
    return page;
}

wxWindow* LauncherFrame::CreateGraphicsPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    const int margin = FromDIP(12), gap = FromDIP(8);

    auto* box = new wxStaticBoxSizer(wxVERTICAL, page, "Picture");
    wxWindow* pb = box->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    resolution_ = ChoiceRow(pb, grid, "&Resolution:",
                            "The size of the road view the game draws, smoothed; the window shows it scaled to fit. "
                            "Lower it if driving stutters on an older computer.");
    for (int r = MIN_RES; r <= MAX_RES; ++r)
        resolution_->Append(wxString::Format(L"%d × %d%s", 320 * r, 200 * r, r == 1 ? " (original)" : ""));
    detail_ = ChoiceRow(pb, grid, "&Sprite detail:",
                        "Maximum: every car, sign and roadside object uses its largest, most detailed picture at "
                        "every distance. By distance: the picture the size of the object calls for.");
    detail_->Append("Maximum");
    detail_->Append("By distance");
    grid->Add(new wxStaticText(pb, wxID_ANY, "&Frame rate:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* rateRow = new wxBoxSizer(wxHORIZONTAL);
    frameRate_ = new wxSpinCtrl(pb, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1),
                                wxSP_ARROW_KEYS, 0, MAX_FRAME_RATE, 60);
    frameRate_->SetToolTip("Frames a second while driving; 0 draws as fast as the computer can.");
    rateRow->Add(frameRate_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    rateRow->Add(GreyText(pb, "frames a second while driving (0 = as fast as possible)"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(rateRow, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(grid, 0, wxALL, gap);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* defaults = new wxButton(page, wxID_ANY, "&Default");
    defaults->SetToolTip("Put every setting back as it was when the launcher was new.");
    auto* apply = new wxButton(page, wxID_APPLY, "&Apply");
    apply->SetToolTip("Keep these settings and go back.");
    auto* cancel = new wxButton(page, wxID_CANCEL, "Cancel");
    cancel->SetToolTip("Go back without changing the settings.");
    buttons->Add(defaults);
    buttons->AddStretchSpacer();
    buttons->Add(apply, 0, wxRIGHT, gap);
    buttons->Add(cancel);
    defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowLook(Look()); });
    apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        look_ = EditedLook();
        SaveLook(look_);
        ClosePage();
    });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ClosePage(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    page->SetSizer(all);
    return page;
}

wxWindow* LauncherFrame::CreateKeysPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    const int margin = FromDIP(12), gap = FromDIP(8), small = FromDIP(4);

    auto* intro = GreyText(page, "The menus and the name entry keep the game's own keys.");

    // A box per group with a label and a key button per action. Driving shares its column with the keys
    // that can't be changed.
    wxStaticBoxSizer* boxes[keys::GROUP_COUNT];
    wxFlexGridSizer* grids[keys::GROUP_COUNT];
    for (int g = 0; g < keys::GROUP_COUNT; ++g) {
        boxes[g] = new wxStaticBoxSizer(wxVERTICAL, page, keys::GROUP_TITLES[g]);
        grids[g] = new wxFlexGridSizer(2, small, gap);
        grids[g]->AddGrowableCol(1);
        boxes[g]->Add(grids[g], 0, wxEXPAND | wxALL, gap);
    }
    for (int i = 0; i < keys::ACTION_COUNT; ++i) {
        const keys::Action& a = keys::ACTIONS[i];
        wxWindow* box = boxes[a.group]->GetStaticBox();
        grids[a.group]->Add(new wxStaticText(box, wxID_ANY, a.label), 0, wxALIGN_CENTER_VERTICAL);
        auto* button = new wxButton(box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(120), -1));
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { ChangeKey(i); });
        grids[a.group]->Add(button, 0, wxEXPAND);
        keyButtons_.push_back(button);
    }
    auto* fixedBox = new wxStaticBoxSizer(wxVERTICAL, page, "Keys that keep their job");
    wxWindow* xb = fixedBox->GetStaticBox();
    auto* fixedGrid = new wxFlexGridSizer(2, small, FromDIP(16));
    for (int i = 0; i < keys::FIXED_COUNT; ++i) {
        fixedGrid->Add(new wxStaticText(xb, wxID_ANY, keys::FIXED[i].keys));
        fixedGrid->Add(GreyText(xb, keys::FIXED[i].what));
    }
    fixedBox->Add(fixedGrid, 0, wxALL, gap);

    auto* first = new wxBoxSizer(wxVERTICAL);
    first->Add(boxes[keys::DRIVING], 0, wxEXPAND);
    first->Add(fixedBox, 0, wxEXPAND | wxTOP, margin);
    auto* columns = new wxBoxSizer(wxHORIZONTAL);
    columns->Add(first, 0, wxEXPAND);
    columns->Add(boxes[keys::GAME], 0, wxEXPAND | wxLEFT, margin);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* defaults = new wxButton(page, wxID_ANY, "&Default");
    defaults->SetToolTip("Put every key back as the game has it.");
    auto* apply = new wxButton(page, wxID_APPLY, "&Apply");
    apply->SetToolTip("Keep these keys and go back.");
    auto* cancel = new wxButton(page, wxID_CANCEL, "Cancel");
    cancel->SetToolTip("Go back without changing the keys.");
    buttons->Add(defaults);
    buttons->AddStretchSpacer();
    buttons->Add(apply, 0, wxRIGHT, gap);
    buttons->Add(cancel);
    defaults->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        edited_ = keys::Defaults();
        ShowEditedKeys();
    });
    apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        bindings_ = edited_;
        keys::Save(bindings_);
        ClosePage();
    });
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ClosePage(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(columns, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    page->SetSizer(all);
    return page;
}

void LauncherFrame::CreateMenus() {
    auto* file = new wxMenu;
    file->Append(wxID_PREFERENCES, wxString::FromUTF8("&Preferences…"));
    file->AppendSeparator();
    file->Append(wxID_EXIT, "E&xit\tAlt+F4");
    auto* settingsMenu = new wxMenu;
    settingsMenu->Append(ID_GRAPHICS, wxString::FromUTF8("&Graphics…"));
    settingsMenu->Append(ID_KEY_BINDINGS, wxString::FromUTF8("&Key Bindings…"));

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(settingsMenu, "&Game settings");
#ifndef __WXMSW__
    auto* about = new wxMenu;
    about->Append(ID_ABOUT, wxString::FromUTF8("&About ") + APP_TITLE + wxString::FromUTF8("…"));
    bar->Append(about, "A&bout");
#endif
    SetMenuBar(bar);
#ifdef __WXMSW__
    // A top-level About item that acts straight away, which wxWidgets menus
    // can't express; MSWWindowProc handles its command.
    const HMENU native = static_cast<HMENU>(bar->GetHMenu());
    AppendMenuW(native, MF_STRING, ID_ABOUT, L"A&bout");
    DrawMenuBar(static_cast<HWND>(GetHWND()));
#endif

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Preferences(); }, wxID_PREFERENCES);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { OpenPage(PAGE_GRAPHICS); }, ID_GRAPHICS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { OpenPage(PAGE_KEYS); }, ID_KEY_BINDINGS);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { About(); }, ID_ABOUT);
}

#ifdef __WXMSW__
WXLRESULT LauncherFrame::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) {
    if (msg == WM_COMMAND && LOWORD(wParam) == ID_ABOUT && lParam == 0) {
        CallAfter([this] { About(); });
        return 0;
    }
    return wxFrame::MSWWindowProc(msg, wParam, lParam);
}
#endif

bool LauncherFrame::AmigaChosen() const { return amigaVersion_->GetValue(); }

void LauncherFrame::ShowVersion() {
    const bool amiga = AmigaChosen();
    diskLabel_->Show(amiga);
    disk_->Show(amiga);
    diskBrowse_->Show(amiga);
    book_->GetPage(PAGE_MAIN)->GetSizer()->Show(amigaBox_, amiga);
    UpdateState();
    if (!loading_ && book_->GetSelection() == PAGE_MAIN) FitPage();
}

void LauncherFrame::UpdateAllBox() {
    int on = 0;
    for (wxCheckBox* f : features_) on += f->GetValue() ? 1 : 0;
    allAmiga_->Set3StateValue(on == 0 ? wxCHK_UNCHECKED : on == AMIGA_FEATURE_COUNT ? wxCHK_CHECKED : wxCHK_UNDETERMINED);
    if (!loading_) UpdateState();
}

void LauncherFrame::UpdateState() {
    const wxString dir = folder_->GetValue(), program = program_->GetValue();
    const bool amiga = AmigaChosen();
    const wxString missing = MissingGameFile(dir);
    const wxString disk = amiga ? FindAmigaDisk(disk_->GetValue()) : wxString();
    const bool haveProgram = wxFileName::FileExists(program);
    bool anyFeature = false;
    for (wxCheckBox* f : features_) anyFeature |= f->GetValue();
    wxString note;
    if (!missing.empty())
        note = wxString::Format("The game folder needs the DOS game's files: %s is missing.", missing);
    else if (amiga && disk.empty())
        note = "The Amiga disk isn't there: choose the .adf, or a folder with it or with the disk's files.";
    else if (!haveProgram)
        note = wxString::Format("%s isn't there.", wxFileName(program).GetFullName());
    else if (amiga && !anyFeature)
        note = "No Amiga feature is checked: the game plays as the PC version.";
    else if (amiga)
        note = wxString::Format("Found TDEGA.EXE and %s.", disk);
    else
        note = "Found TDEGA.EXE.";
    const bool ok = missing.empty() && haveProgram && (!amiga || !disk.empty());
    statusIcon_->Show(!ok);
    statusNote_->SetLabel(note);
    play_->Enable(ok);
    play_->GetParent()->Layout();
}

void LauncherFrame::Browse(wxTextCtrl* text, bool folder, const wxString& title) {
    if (folder) {
        wxDirDialog dialog(this, title, text->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) text->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
        return;
    }
    wxFileName current(text->GetValue());
    const bool disk = text == disk_;
#ifdef __WXMSW__
    const char* const filter = disk ? "Amiga disk images (*.adf)|*.adf;*.ADF|All files (*.*)|*.*"
                                    : "Programs (*.exe)|*.exe|All files (*.*)|*.*";
#else
    const char* const filter = disk ? "Amiga disk images (*.adf)|*.adf;*.ADF|All files|*" : "All files|*";
#endif
    const wxString start = wxFileName::DirExists(current.GetFullPath()) ? current.GetFullPath() : current.GetPath();
    wxFileDialog dialog(this, title, start, current.GetFullName(), filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) text->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherFrame::Play() {
    Save();
    GameOptions options;
    options.program = wxFileName(program_->GetValue()).GetFullPath();
    options.gameDir = wxFileName(folder_->GetValue()).GetFullPath();
    options.scale = scale_->GetSelection() + MIN_SCALE;
    options.fullscreen = fullscreen_->GetValue();
    options.biosKeys = biosKeys_->GetValue();
    options.resScale = look_.resScale;
    options.spriteMax = look_.spriteMax;
    options.frameRate = look_.frameRate;
    options.keys = keys::Argument(bindings_);
    if (AmigaChosen()) {
        for (int i = 0; i < AMIGA_FEATURE_COUNT; ++i) {
            if (!features_[i]->GetValue()) continue;
            if (!options.amiga.empty()) options.amiga += ",";
            options.amiga += AMIGA_FEATURES[i].name;
        }
        options.amigaDisk = wxFileName(disk_->GetValue()).GetFullPath();
    }
    wxString error;
    if (!LaunchGame(options, error)) wxMessageBox(error, APP_TITLE, wxOK | wxICON_ERROR, this);
}

void LauncherFrame::Save() {
    SavePath("GameFolder", folder_->GetValue(), DefaultGameDir());
    SavePath("AmigaDisk", disk_->GetValue(), DefaultAmigaDisk());
    SavePath("Program", program_->GetValue(), DefaultProgram());
    settings::SetString(SECTION, "Version", AmigaChosen() ? "amiga" : "pc");
    settings::SetInt(SECTION, "Scale", scale_->GetSelection() + MIN_SCALE);
    settings::SetInt(SECTION, "Fullscreen", fullscreen_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "BiosKeys", biosKeys_->GetValue() ? 1 : 0);
    for (int i = 0; i < AMIGA_FEATURE_COUNT; ++i)
        settings::SetInt(SECTION, AMIGA_FEATURES[i].setting, features_[i]->GetValue() ? 1 : 0);
    SaveLook(look_);
    settings::SaveWindowPosition(SECTION, this);
}

void LauncherFrame::Preferences() {
    wxDialog dialog(this, wxID_ANY, "Preferences");
    auto* all = new wxBoxSizer(wxVERTICAL);
    auto* topmost = new wxCheckBox(&dialog, wxID_ANY, "Always on &top");
    topmost->SetValue(topmost_);
    topmost->SetToolTip("Keep the launcher above other windows.");
    all->Add(topmost, 0, wxALL, dialog.FromDIP(12));
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
             dialog.FromDIP(12));
    dialog.SetSizerAndFit(all);
    dialog.SetMinSize(wxSize(dialog.FromDIP(300), -1));
    dialog.Fit();
    dialog.CentreOnParent();
    topmost->SetFocus();
    if (dialog.ShowModal() != wxID_OK) return;
    if (topmost->GetValue() != topmost_) SetTopmost(topmost->GetValue());
}

void LauncherFrame::SetTopmost(bool on) {
    topmost_ = on;
    const long style = GetWindowStyleFlag();
    SetWindowStyleFlag(on ? style | wxSTAY_ON_TOP : style & ~wxSTAY_ON_TOP);
    settings::SetInt("Preferences", "AlwaysOnTop", on ? 1 : 0);
}

// The window takes the size of the page shown (the book on its own would take the largest page's).
void LauncherFrame::FitPage() {
    wxWindow* page = book_->GetCurrentPage();
    page->Layout();
    SetClientSize(page->GetBestSize());
}

void LauncherFrame::OpenPage(int page) {
    if (book_->GetSelection() == page) return;
    if (page == PAGE_GRAPHICS) ShowLook(look_);
    if (page == PAGE_KEYS) {
        edited_ = bindings_;
        ShowEditedKeys();
    }
    book_->ChangeSelection(page);
    FitPage();
    GetMenuBar()->Enable(ID_GRAPHICS, false);
    GetMenuBar()->Enable(ID_KEY_BINDINGS, false);
    SetDefaultItem(book_->GetPage(page)->FindWindow(wxID_APPLY));
    if (page == PAGE_GRAPHICS) resolution_->SetFocus();
    else keyButtons_.front()->SetFocus();
}

void LauncherFrame::ClosePage() {
    book_->ChangeSelection(PAGE_MAIN);
    FitPage();
    GetMenuBar()->Enable(ID_GRAPHICS, true);
    GetMenuBar()->Enable(ID_KEY_BINDINGS, true);
    SetDefaultItem(play_);
    UpdateState();
    play_->SetFocus();
}

void LauncherFrame::ShowLook(const Look& look) {
    resolution_->SetSelection(look.resScale - MIN_RES);
    detail_->SetSelection(look.spriteMax ? 0 : 1);
    frameRate_->SetValue(look.frameRate);
}

Look LauncherFrame::EditedLook() const {
    Look look;
    look.resScale = resolution_->GetSelection() + MIN_RES;
    look.spriteMax = detail_->GetSelection() == 0;
    look.frameRate = frameRate_->GetValue();
    return look;
}

void LauncherFrame::ShowEditedKeys() {
    for (int i = 0; i < keys::ACTION_COUNT; ++i) {
        const wxString name = keys::Name(edited_[i]);
        // Buttons treat '&' as a mnemonic marker.
        wxString label = name;
        label.Replace("&", "&&");
        keyButtons_[i]->SetLabel(label);
        keyButtons_[i]->SetToolTip(wxString::Format("%s: %s. Click to change it.", keys::ACTIONS[i].label, name));
        // A key that differs from the game's own stands out.
        wxFont font = keyButtons_[i]->GetParent()->GetFont();
        if (edited_[i] != keys::ACTIONS[i].def) font.MakeBold();
        keyButtons_[i]->SetFont(font);
    }
    book_->GetPage(PAGE_KEYS)->Layout();
}

void LauncherFrame::ChangeKey(int action) {
    const keys::Action& a = keys::ACTIONS[action];
    const int key = keys::AskKey(this, a.label);
    if (key < 0 || key == edited_[action]) return;
    if (key != 0) {
        for (int other = 0; other < keys::ACTION_COUNT; ++other) {
            if (other == action || edited_[other] != key) continue;
            const wxString question = wxString::Format(
                "%s is already the key for %s.\n\nUse it for %s instead? %s will have no key.", keys::Name(key),
                keys::ACTIONS[other].label, a.label, keys::ACTIONS[other].label);
            if (wxMessageBox(question, "Key Bindings", wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
            edited_[other] = 0;
        }
    }
    edited_[action] = key;
    ShowEditedKeys();
}

void LauncherFrame::About() {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
    const wxString blurb = "Starts testdrive-enhanced, Test Drive (Accolade, 1987) on SDL3 with a smooth, "
                           "high-resolution road view, and the Amiga release's features to mix in.";
#ifdef __WXMSW__
    // The Windows task dialog.
    const wxString content = wxString::Format(
        "%s\n\n"
        "Author: Krzysztof Kania\n"
        "Website: <a href=\"%s\">kkania.com</a>\n"
        "Source: <a href=\"%s\">github.com/kylofon/testdrive-enhanced</a>\n"
        "Support: <a href=\"%s\">buymeacoffee.com/krzysztofkania</a>",
        blurb, WEBSITE, SOURCE, SUPPORT);
    wxIcon icon;
    icon.CopyFromBitmap(AppBitmap(FromDIP(32)));
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = static_cast<HWND>(GetHWND());
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.wc_str();
    dialog.hMainIcon = static_cast<HICON>(icon.GetHICON());
    dialog.pszMainInstruction = heading.wc_str();
    dialog.pszContent = content.wc_str();
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
#else
    wxDialog dialog(this, wxID_ANY, title);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(&dialog, wxID_ANY, AppBitmap(dialog.FromDIP(48))), 0, wxALL, dialog.FromDIP(12));

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto* headingText = new wxStaticText(&dialog, wxID_ANY, heading);
    headingText->SetFont(dialog.GetFont().Bold().Scaled(1.3f));
    text->Add(headingText, 0, wxBOTTOM, dialog.FromDIP(8));
    text->Add(new wxStaticText(&dialog, wxID_ANY, blurb), 0, wxBOTTOM, dialog.FromDIP(12));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "Author: Krzysztof Kania"));
    AddLink(&dialog, text, "Website:", "kkania.com", WEBSITE);
    AddLink(&dialog, text, "Source:", "github.com/kylofon/testdrive-enhanced", SOURCE);
    AddLink(&dialog, text, "Support:", "buymeacoffee.com/krzysztofkania", SUPPORT);
    body->Add(text, 1, wxTOP | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(body, 1, wxEXPAND);
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    dialog.SetSizerAndFit(all);
    dialog.CentreOnParent();
    dialog.ShowModal();
#endif
}
