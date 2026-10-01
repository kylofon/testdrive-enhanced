// launcher.h -- the launcher window: the version (PC or Amiga), the game folders, the options and, for the
// Amiga, the Amiga features to mix in, then Play. Game settings > Graphics and Game settings > Key Bindings
// are shown in the window in its place.
#pragma once

#include <wx/frame.h>

#include <vector>

#include "keys.h"

class wxButton;
class wxCheckBox;
class wxChoice;
class wxPanel;
class wxRadioButton;
class wxSimplebook;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticBoxSizer;
class wxStaticText;
class wxTextCtrl;

extern const char* const APP_TITLE;

// Game settings > Graphics.
struct Look {
    int resScale = 4;        // --res-scale
    bool spriteMax = true;   // --sprite-detail max / auto
    int frameRate = 60;      // --frame-rate
};

class LauncherFrame : public wxFrame {
public:
    LauncherFrame();

private:
    wxWindow* CreateMainPage(wxWindow* parent);
    wxWindow* CreateGraphicsPage(wxWindow* parent);
    wxWindow* CreateKeysPage(wxWindow* parent);
    void CreateMenus();
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    bool AmigaChosen() const;
    // Shows the Amiga disk and the Amiga features for the Amiga version.
    void ShowVersion();
    // Checks the folders and the program and enables Play to match.
    void UpdateState();
    // The "All Amiga features" box from the features: on, off or some.
    void UpdateAllBox();
    void Browse(wxTextCtrl* text, bool folder, const wxString& title);
    void Play();
    void About();
    void Preferences();
    void SetTopmost(bool on);
    void Save();

    // The pages shown in place of the main one: Graphics and Key Bindings.
    void OpenPage(int page);
    void ClosePage();
    void FitPage();
    void ShowLook(const Look& look);
    Look EditedLook() const;
    void ChangeKey(int action);
    void ShowEditedKeys();

    wxSimplebook* book_ = nullptr;
    wxRadioButton* pc_ = nullptr;
    wxRadioButton* amigaVersion_ = nullptr;
    wxTextCtrl* folder_ = nullptr;
    wxStaticText* diskLabel_ = nullptr;
    wxTextCtrl* disk_ = nullptr;
    wxButton* diskBrowse_ = nullptr;
    wxTextCtrl* program_ = nullptr;
    wxStaticBitmap* statusIcon_ = nullptr;
    wxStaticText* statusNote_ = nullptr;
    wxChoice* scale_ = nullptr;
    wxCheckBox* fullscreen_ = nullptr;
    wxCheckBox* biosKeys_ = nullptr;
    wxStaticBoxSizer* amigaBox_ = nullptr;
    wxCheckBox* allAmiga_ = nullptr;
    std::vector<wxCheckBox*> features_;   // in the order of AMIGA_FEATURES
    wxButton* play_ = nullptr;

    // The Graphics page.
    wxChoice* resolution_ = nullptr;
    wxChoice* detail_ = nullptr;
    wxSpinCtrl* frameRate_ = nullptr;
    Look look_;  // saved

    // The Key Bindings page: one button per action.
    std::vector<wxButton*> keyButtons_;
    keys::Bindings bindings_;  // saved
    keys::Bindings edited_;    // on the Key Bindings page

    bool topmost_ = false;
    bool loading_ = true;  // no edits are recorded while the window is built
};
