// game.cpp -- the files the game needs, the Amiga features and starting the game.
#include "game.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <string>
#include <vector>

const AmigaFeature AMIGA_FEATURES[] = {
    {"intro", "AmigaIntro", "Amiga &intro", "The Amiga title sequence instead of the DOS one."},
    {"menu", "AmigaMenu", "Amiga &menu", "The Amiga car selection, showroom and play-again menu."},
    {"dashboards", "AmigaDashboards", "Amiga &dashboards",
     "The Amiga cockpits: each car's dashboard, gauges, steering wheel and gear knob, with the roof, the window "
     "post, the hood and the mirror; the road fills the screen above them."},
    {"music", "AmigaMusic", "Amiga m&usic", "The Amiga songs, on an emulated Paula, instead of the PC speaker's."},
    {"projection", "AmigaProjection", "Amiga &projection",
     "The road's shape as the Amiga draws it: gentler bends and crests, turning up to 90 degrees."},
    {"sky", "AmigaSky", "Amiga s&ky", "The Amiga's blue sky with drifting clouds instead of the mountains."},
    {"cliffs", "AmigaCliffs", "Amiga &cliffs", "The Amiga's brown rock face."},
    {"traffic", "AmigaTraffic", "Amiga &traffic speed",
     "The traffic drives as on the Amiga: at the speed limit, the RX-7 a quarter faster, its own speeds before "
     "the first sign, and slower in the outer lane of a wide road."},
    {"cops", "AmigaCops", "Amiga c&ops",
     "The Amiga police: a radar trap goes off at more than 15 mph over the limit, and the patrol car comes from "
     "further back at a steady 108 mph or more, tails you, passes you and stops you, but never near the end of a "
     "stage."},
    {"wide-roads", "AmigaWideRoads", "Amiga &wide roads",
     "The passing lanes of the first two stages, which the DOS version leaves out: the road widens to two "
     "lanes each way with lane marks."},
    {"signs", "AmigaSigns", "Amiga road &signs",
     "The Amiga's road signs and poles, with its speed limits of 30, 55 and 65 mph and the passing-lane and "
     "merge signs."},
};
const int AMIGA_FEATURE_COUNT = static_cast<int>(sizeof AMIGA_FEATURES / sizeof AMIGA_FEATURES[0]);

namespace {

// A file of `dir` whose name matches `name` as written, in upper or in lower case (case matters outside
// Windows); empty if there is none.
wxString FindFile(const wxString& dir, const wxString& name) {
    for (const wxString& n : {name, name.Upper(), name.Lower()}) {
        const wxString path = wxFileName(dir, n).GetFullPath();
        if (wxFileName::FileExists(path)) return path;
    }
    return wxString();
}

}  // namespace

wxString LauncherDir() { return wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath(); }

wxString DefaultGameDir() { return wxFileName(LauncherDir(), "Game").GetFullPath(); }
wxString DefaultAmigaDisk() { return wxFileName(LauncherDir(), "Game Amiga").GetFullPath(); }

wxString DefaultProgram() {
    wxFileName name(LauncherDir(), "testdrive-enhanced");
#ifdef __WXMSW__
    name.SetExt("exe");
#endif
    return name.GetFullPath();
}

wxString MissingGameFile(const wxString& dir) {
    if (dir.empty() || !wxFileName::DirExists(dir)) return "TDEGA.EXE";
    for (const char* name : {"TDEGA.EXE", "CARS.TXT", "TDSND.SND"})
        if (FindFile(dir, name).empty()) return name;
    wxDir folder(dir);
    wxString pes;
    if (!folder.IsOpened() || !(folder.GetFirst(&pes, "*.PES", wxDIR_FILES) || folder.GetFirst(&pes, "*.pes", wxDIR_FILES)))
        return "the *.PES files";
    return wxString();
}

wxString FindAmigaDisk(const wxString& path) {
    if (path.empty()) return wxString();
    if (wxFileName::FileExists(path)) {
        wxFileName name(path);
        return name.GetExt().CmpNoCase("adf") == 0 ? name.GetFullName() : wxString();
    }
    if (!wxFileName::DirExists(path)) return wxString();
    if (!FindFile(path, "td").empty()) return "td";
    wxDir folder(path);
    wxString adf;
    if (folder.IsOpened() && (folder.GetFirst(&adf, "*.adf", wxDIR_FILES) || folder.GetFirst(&adf, "*.ADF", wxDIR_FILES)))
        return adf;
    return wxString();
}

bool LaunchGame(const GameOptions& o, wxString& error) {
    if (!wxFileName::FileExists(o.program)) {
        error = wxString::Format("The game's program isn't there:\n\n%s", o.program);
        return false;
    }
    std::vector<wxString> args{o.program,
                               "--game-dir", o.gameDir,
                               "--scale", wxString::Format("%d", o.scale),
                               "--res-scale", wxString::Format("%d", o.resScale),
                               "--sprite-detail", o.spriteMax ? "max" : "auto",
                               "--frame-rate", wxString::Format("%d", o.frameRate)};
    if (o.fullscreen) args.push_back("--fullscreen");
    if (o.biosKeys) args.push_back("--bios-keys");
    if (!o.keys.empty()) args.insert(args.end(), {"--keys", o.keys});
    if (!o.amiga.empty()) args.insert(args.end(), {"--amiga", o.amiga, "--amiga-disk", o.amigaDisk});

    std::vector<std::wstring> wide;
    for (const wxString& a : args) wide.push_back(a.ToStdWstring());
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env;  // an empty variable map: the game inherits the launcher's environment
    env.cwd = wxFileName(o.program).GetPath();
    long pid;
    {
        wxLogNull quiet;  // wxExecute would show its own error box
        pid = wxExecute(argv.data(), wxEXEC_ASYNC, nullptr, &env);
    }
    if (pid == 0) {
        error = wxString::Format("Couldn't start %s.\n\n%s", wxFileName(o.program).GetFullName(),
                                 wxSysErrorMsgStr(wxSysErrorCode()));
        return false;
    }
    return true;
}
