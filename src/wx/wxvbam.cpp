// mainline:
//   parse cmd line
//   load xrc file (guiinit.cpp does most of instantiation)
//   create & display main frame

#include "wxvbam.h"

#ifdef __WXMSW__
#include <windows.h>
#endif

#include <stdio.h>
#include <wx/cmdline.h>
#include <wx/display.h>
#include <wx/file.h>
#include <wx/filesys.h>
#include <wx/fs_arc.h>
#include <wx/fs_mem.h>
#include <wx/menu.h>
#include <wx/mstream.h>
#include <wx/progdlg.h>
#include <wx/protocol/http.h>
#include <wx/regex.h>
#include <wx/sstream.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/txtstrm.h>
#include <wx/url.h>
#include <wx/wfstream.h>
#include <wx/wxcrtvararg.h>
#include <wx/zipstrm.h>

#include "../gba/remote.h"

// The built-in xrc file
#include "builtin-xrc.h"

// The built-in vba-over.ini
#include "builtin-over.h"
#include "config/game-control.h"
#include "config/option-proxy.h"
#include "config/option.h"
#include "config/user-input.h"
#include "strutils.h"
#include "wayland.h"
#include "widgets/group-check-box.h"
#include "widgets/user-input-ctrl.h"
#include "wxhead.h"

namespace {

// Resets the accelerator text for `menu_item` to the first keyboard input.
void ResetMenuItemAccelerator(wxMenuItem* menu_item) {
    const wxString old_label = menu_item->GetItemLabel();
    const size_t tab_index = old_label.find('\t');
    wxString new_label;
    new_label = old_label;
    if (tab_index != wxString::npos) {
        new_label.resize(tab_index);
    }
    std::set<config::UserInput> user_inputs =
        gopts.shortcuts.InputsForCommand(menu_item->GetId());
    for (const config::UserInput& user_input : user_inputs) {
        if (user_input.device() != config::UserInput::Device::Keyboard) {
            // Cannot use joystick keybinding as text without wx assertion error.
            continue;
        }

        new_label.append('\t');
        new_label.append(user_input.ToLocalizedString());
        break;
    }

    if (old_label != new_label) {
        menu_item->SetItemLabel(new_label);
    }
}

static const wxString kOldConfigFileName("vbam.conf");
static const wxString knewConfigFileName("vbam.ini");
static const char kDotDir[] = "visualboyadvance-m";

}  // namespace

#ifndef NO_DEBUGGER
void(*dbgMain)() = remoteStubMain;
void(*dbgSignal)(int, int) = remoteStubSignal;
void(*dbgOutput)(const char *, uint32_t) = debuggerOutput;
#endif

#ifdef __WXMSW__

int __stdcall WinMain(HINSTANCE hInstance,
                      HINSTANCE hPrevInstance,
                      LPSTR lpCmdLine,
                      int nCmdShow) {
    bool console_attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
#ifdef DEBUG
    // In debug builds, create a console if none is attached.
    if (!console_attached) {
        console_attached = AllocConsole() != FALSE;
    }
#endif  // DEBUG

    // Redirect stdout/stderr to the console if one is attached.
    // This code was taken from Dolphin.
    // https://github.com/dolphin-emu/dolphin/blob/6cf99195c645f54d54c72322ad0312a0e56bc985/Source/Core/DolphinQt/Main.cpp#L112
    HANDLE stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (console_attached && stdout_handle) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    // Set up logging.
#ifdef DEBUG
    wxLog::SetLogLevel(wxLOG_Trace);
#else   // DEBUG
    wxLog::SetLogLevel(wxLOG_Info);
#endif  // DEBUG

    // Redirect e.g. --help to stderr.
    wxMessageOutput::Set(new wxMessageOutputStderr());

    // This will be freed on wxEntry exit.
    wxApp::SetInstance(new wxvbamApp());
    return wxEntry(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}

#else  // __WXMSW__

int main(int argc, char** argv) {
    // Set up logging.
#ifdef DEBUG
    wxLog::SetLogLevel(wxLOG_Trace);
#else   // DEBUG
    wxLog::SetLogLevel(wxLOG_Info);
#endif  // DEBUG

    // This will be freed on wxEntry exit.
    wxApp::SetInstance(new wxvbamApp());
    return wxEntry(argc, argv);
}

#endif  // __WXMSW__

wxvbamApp& wxGetApp() {
    return *static_cast<wxvbamApp*>(wxApp::GetInstance());
}

IMPLEMENT_DYNAMIC_CLASS(MainFrame, wxFrame)

#ifndef NO_ONLINEUPDATES
#include "autoupdater/autoupdater.h"
#endif // NO_ONLINEUPDATES

// Initializer for struct cmditem
cmditem new_cmditem(const wxString cmd, const wxString name, int cmd_id,
                    int mask_flags, wxMenuItem* mi)
{
    struct cmditem tmp = {cmd, name, cmd_id, mask_flags, mi};
    return tmp;
}

// generate config file path
static void get_config_path(wxPathList& path, bool exists = true)
{
    wxString current_app_name = wxGetApp().GetAppName();

    //   local config dir first, then global
    //   locale-specific res first, then main
    wxStandardPathsBase& stdp = wxStandardPaths::Get();
#define add_path(p)                                                                                          \
    do {                                                                                                     \
        const wxString& s = stdp.p;                                                                          \
        wxFileName parent = wxFileName::DirName(s + wxT("//.."));                                            \
        parent.MakeAbsolute();                                                                               \
        if ((wxDirExists(s) && wxIsWritable(s)) || ((!exists || !wxDirExists(s)) && parent.IsDirWritable())) \
            path.Add(s);                                                                                     \
    } while (0)
#define add_nonstandard_path(p)                                                                              \
    do {                                                                                                     \
        const wxString& s = p;                                                                               \
        wxFileName parent = wxFileName::DirName(s + wxT("//.."));                                            \
        parent.MakeAbsolute();                                                                               \
        if ((wxDirExists(s) && wxIsWritable(s)) || ((!exists || !wxDirExists(s)) && parent.IsDirWritable())) \
            path.Add(s);                                                                                     \
    } while (0)

    static bool debug_dumped = false;

    if (!debug_dumped) {
        wxLogDebug(wxT("GetUserLocalDataDir(): %s"), stdp.GetUserLocalDataDir().c_str());
        wxLogDebug(wxT("GetUserDataDir(): %s"), stdp.GetUserDataDir().c_str());
        wxLogDebug(wxT("GetLocalizedResourcesDir(wxGetApp().locale.GetCanonicalName()): %s"), stdp.GetLocalizedResourcesDir(wxGetApp().locale.GetCanonicalName()).c_str());
        wxLogDebug(wxT("GetResourcesDir(): %s"), stdp.GetResourcesDir().c_str());
        wxLogDebug(wxT("GetDataDir(): %s"), stdp.GetDataDir().c_str());
        wxLogDebug(wxT("GetLocalDataDir(): %s"), stdp.GetLocalDataDir().c_str());
        wxLogDebug(wxT("plugins_dir: %s"), wxGetApp().GetPluginsDir().c_str());
        wxLogDebug(wxT("XdgConfigDir: %s"), (wxString(get_xdg_user_config_home().c_str(), wxConvLibc) + current_app_name).c_str());
        debug_dumped = true;
    }

// When native support for XDG dirs is available (wxWidgets >= 3.1),
// this will be no longer necessary
#if defined(__WXGTK__)
    // XDG spec manual support
    // ${XDG_CONFIG_HOME:-$HOME/.config}/`appname`
    wxString old_config = wxString(getenv("HOME"), wxConvLibc) + wxT(FILE_SEP) + wxT(".vbam");
    wxString new_config(get_xdg_user_config_home().c_str(), wxConvLibc);
    if (!wxDirExists(old_config) && wxIsWritable(new_config))
    {
        wxFileName new_path(new_config, wxEmptyString);
        new_path.AppendDir(current_app_name);
        new_path.MakeAbsolute();

        add_nonstandard_path(new_path.GetFullPath());
    }
    else
    {
        // config is in $HOME/.vbam/
        add_nonstandard_path(old_config);
    }
#endif

    // NOTE: this does not support XDG (freedesktop.org) paths
    add_path(GetUserLocalDataDir());
    add_path(GetUserDataDir());
    add_path(GetLocalizedResourcesDir(wxGetApp().locale.GetCanonicalName()));
    add_path(GetResourcesDir());
    add_path(GetDataDir());
    add_path(GetLocalDataDir());
    add_nonstandard_path(wxGetApp().GetPluginsDir());
}

static void tack_full_path(wxString& s, const wxString& app = wxEmptyString)
{
    // regenerate path, including nonexistent dirs this time
    wxPathList full_config_path;
    get_config_path(full_config_path, false);

    for (size_t i = 0; i < full_config_path.size(); i++)
        s += wxT("\n\t") + full_config_path[i] + app;
}

const wxString wxvbamApp::GetPluginsDir()
{
    return wxStandardPaths::Get().GetPluginsDir();
}

wxString wxvbamApp::GetConfigurationPath() {
    // first check if config files exists in reverse order
    // (from system paths to more local paths.)
    if (data_path.empty()) {
        get_config_path(config_path);

        for (int i = config_path.size() - 1; i >= 0; i--) {
            wxFileName fn(config_path[i], knewConfigFileName);

            if (fn.FileExists() && fn.IsFileWritable()) {
                data_path = config_path[i];
                break;
            }
        }
    }

    // if no config file was not found, search for writable config
    // dir or parent to create it in in OnInit in normal order
    // (from user paths to system paths.)
    if (data_path.empty()) {
        for (size_t i = 0; i < config_path.size(); i++) {
            // Check if path is writeable
            if (wxIsWritable(config_path[i])) {
                data_path = config_path[i];
                break;
            }

            // check if parent of path is writable, so we can
            // create the path in OnInit
            wxFileName parent_dir = wxFileName::DirName(config_path[i] + wxT("//.."));
            parent_dir.MakeAbsolute();

            if (parent_dir.IsDirWritable()) {
                data_path = config_path[i];
                break;
            }
        }
    }

    return data_path;
}

wxString wxvbamApp::GetAbsolutePath(wxString path)
{
    wxFileName fn(path);

    if (fn.IsRelative()) {
        fn.MakeRelativeTo(GetConfigurationPath());
        fn.Normalize();
        return fn.GetFullPath();
    }

    return path;
}

bool wxvbamApp::OnInit() {
    using_wayland = IsWayland();

    // use consistent names for config
    SetAppName(_("visualboyadvance-m"));
#if (wxMAJOR_VERSION >= 3)
    SetAppDisplayName(_T("VisualBoyAdvance-M"));
#endif
    // load system default locale, if available
    locale.Init();
    locale.AddCatalog(_T("wxvbam"));
    // make built-in xrc file available
    // this has to be done before parent OnInit() so xrc dump works
    wxFileSystem::AddHandler(new wxMemoryFSHandler);
    wxFileSystem::AddHandler(new wxArchiveFSHandler);
    wxMemoryFSHandler::AddFileWithMimeType(wxT("wxvbam.xrs"), builtin_xrs, sizeof(builtin_xrs), wxT("application/zip"));

    if (!wxApp::OnInit())
        return false;

    if (console_mode)
        return true;

    // prepare for loading xrc files
    wxXmlResource* xr = wxXmlResource::Get();
    // note: if linking statically, next 2 pull in lot of unused code
    // maybe in future if not wxSHARED, load only builtin-needed handlers
    xr->InitAllHandlers();
    xr->AddHandler(new widgets::GroupCheckBoxXmlHandler());
    xr->AddHandler(new widgets::UserInputCtrlXmlHandler());
    wxInitAllImageHandlers();
    get_config_path(config_path);
    // first, load override xrcs
    // this can only override entire root nodes
    // 2.9 has LoadAllFiles(), but this is 2.8, so we'll do it manually
    wxString cwd = wxGetCwd();

    for (size_t i = 0; i < config_path.size(); i++)
        if (wxDirExists(config_path[i]) && wxSetWorkingDirectory(config_path[i])) {
            // *.xr[cs] doesn't work (double the number of scans)
            // 2.9 gives errors for no files found, so manual precheck needed
            // (yet another double the number of scans)
            if (!wxFindFirstFile(wxT("*.xrc")).empty())
                xr->Load(wxT("*.xrc"));

            if (!wxFindFirstFile(wxT("*.xrs")).empty())
                xr->Load(wxT("*.xrs"));
        }

    wxFileName xrcDir(GetConfigurationPath() + wxT("//xrc"), wxEmptyString);

    if (xrcDir.DirExists() && wxSetWorkingDirectory(xrcDir.GetFullPath()) && !wxFindFirstFile(wxT("*.xrc")).empty()) {
        xr->Load(wxT("*.xrc"));
    } else {
        // finally, load built-in xrc
        xr->Load(wxT("memory:wxvbam.xrs"));
    }

    wxSetWorkingDirectory(cwd);

    if (!config_file_.IsOk()) {
        // Set up the default configuration file.
        // This needs to be in a subdir to support other config as well.
        // NOTE: this does not support XDG (freedesktop.org) paths.
        // We rely on wx to build the paths in a cross-platform manner. However,
        // the wxFileName APIs are weird and don't quite work as intended so we
        // use the wxString APIs for files instead.
        const wxString old_conf_file(
            wxFileName(GetConfigurationPath(), kOldConfigFileName)
                .GetFullPath());
        const wxString new_conf_file(
            wxFileName(GetConfigurationPath(), knewConfigFileName)
                .GetFullPath());

        if (wxDirExists(new_conf_file)) {
            wxLogError(_("Invalid configuration file provided: %s"),
                       new_conf_file);
            return false;
        }

        // /MIGRATION
        // Migrate from 'vbam.conf' to 'vbam.ini' to manage a single config
        // file for all platforms.
        if (!wxFileExists(new_conf_file) && wxFileExists(old_conf_file)) {
            wxRenameFile(old_conf_file, new_conf_file, false);
        }
        // /END_MIGRATION

        config_file_ = new_conf_file;
    }

    if (!config_file_.IsOk() || wxDirExists(config_file_.GetFullPath())) {
        wxLogError(_("Invalid configuration file provided: %s"),
                   config_file_.GetFullPath());
        return false;
    }

    // wx takes ownership of the wxFileConfig here. It will be deleted on app
    // destruction.
    wxConfigBase::DontCreateOnDemand();
    wxConfigBase::Set(new wxFileConfig("vbam", wxEmptyString,
                                       config_file_.GetFullPath(),
                                       wxEmptyString, wxCONFIG_USE_LOCAL_FILE));

    // wx does not create the directories by itself so do it here, if needed.
    if (!wxDirExists(config_file_.GetPath())) {
       config_file_.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    // Load the default options.
    load_opts(!config_file_.Exists());

    // wxGLCanvas segfaults under wayland before wx 3.2
#if defined(HAVE_WAYLAND_SUPPORT) && !defined(HAVE_WAYLAND_EGL)
    if (UsingWayland()) {
        OPTION(kDispRenderMethod) = config::RenderMethod::kSimple;
    }
#endif

    // process command-line options
    for (size_t i = 0; i < pending_optset.size(); i++) {
        auto parts = strutils::split(pending_optset[i], wxT('='));
        opt_set(parts[0], parts[1]);
    }

    pending_optset.clear();
    wxFileName vba_over(GetConfigurationPath(), wxT("vba-over.ini"));
    wxFileName rdb(GetConfigurationPath(), wxT("Nintendo - Game Boy Advance*.dat"));
    wxFileName scene_rdb(GetConfigurationPath(), wxT("Nintendo - Game Boy Advance (Scene)*.dat"));
    wxFileName nointro_rdb(GetConfigurationPath(), wxT("Official No-Intro Nintendo Gameboy Advance Number (Date).xml"));
    wxString f = wxFindFirstFile(nointro_rdb.GetFullPath(), wxFILE);

    if (!f.empty() && wxFileName(f).IsFileReadable())
        rom_database_nointro = f;

    f = wxFindFirstFile(scene_rdb.GetFullPath(), wxFILE);

    if (!f.empty() && wxFileName(f).IsFileReadable())
        rom_database_scene = f;

    f = wxFindFirstFile(rdb.GetFullPath(), wxFILE);

    while (!f.empty()) {
        if (f == rom_database_scene.GetFullPath()) {
            f = wxFindNextFile();
        } else if (wxFileName(f).IsFileReadable()) {
            rom_database = f;
            break;
        }
    }

    // load vba-over.ini
    // rather than dealing with wxConfig's broken search path, just use
    // the same one that the xrc overrides use
    // this also allows us to override a group at a time, add commments, and
    // add the file from which the group came
    wxMemoryInputStream mis(builtin_over, sizeof(builtin_over));
    overrides = new wxFileConfig(mis);
    wxRegEx cmtre;
    // not the most efficient thing to do: read entire file into a string
    // just to parse the comments out
    wxString bovs((const char*)builtin_over, wxConvUTF8, sizeof(builtin_over));
    bool cont;
    wxString s;
    long grp_idx;
#define CMT_RE_START wxT("(^|[\n\r])# ?([^\n\r]*)(\r?\n|\r)\\[")

    for (cont = overrides->GetFirstGroup(s, grp_idx); cont;
         cont = overrides->GetNextGroup(s, grp_idx)) {
        // apparently even MacOSX sometimes uses the old \r by itself
        wxString cmt(CMT_RE_START);
        cmt += s + wxT("\\]");

        if (cmtre.Compile(cmt) && cmtre.Matches(bovs))
            cmt = cmtre.GetMatch(bovs, 2);
        else
            cmt = wxEmptyString;

        overrides->Write(s + wxT("/comment"), cmt);
    }

    if (vba_over.FileExists()) {
        wxStringOutputStream sos;
        wxFileInputStream fis(vba_over.GetFullPath());
        // not the most efficient thing to do: read entire file into a string
        // just to parse the comments out
        fis.Read(sos);
        // rather than assuming the file is seekable, use the string we just
        // read as an input stream
        wxStringInputStream sis(sos.GetString());
        wxFileConfig ov(sis);

        for (cont = ov.GetFirstGroup(s, grp_idx); cont;
             cont = ov.GetNextGroup(s, grp_idx)) {
            overrides->DeleteGroup(s);
            overrides->SetPath(s);
            ov.SetPath(s);
            overrides->Write(wxT("path"), GetConfigurationPath());
            // apparently even MacOSX sometimes uses \r by itself
            wxString cmt(CMT_RE_START);
            cmt += s + wxT("\\]");

            if (cmtre.Compile(cmt) && cmtre.Matches(sos.GetString()))
                cmt = cmtre.GetMatch(sos.GetString(), 2);
            else
                cmt = wxEmptyString;

            overrides->Write(wxT("comment"), cmt);
            long ent_idx;

            for (cont = ov.GetFirstEntry(s, ent_idx); cont;
                 cont = ov.GetNextEntry(s, ent_idx))
                overrides->Write(s, ov.Read(s, wxEmptyString));

            ov.SetPath(wxT("/"));
            overrides->SetPath(wxT("/"));
        }
    }

    // Initialize game bindings here, after defaults bindings, vbam.ini bindings
    // and command line overrides have been applied.
    config::GameControlState::Instance().OnGameBindingsChanged();

    // We need to gather this information before crating the MainFrame as the
    // OnSize / OnMove event handlers can fire during construction.
    const wxRect client_rect(
        OPTION(kGeomWindowX).Get(),
        OPTION(kGeomWindowY).Get(),
        OPTION(kGeomWindowWidth).Get(),
        OPTION(kGeomWindowHeight).Get());
    const bool is_fullscreen = OPTION(kGeomFullScreen);
    const bool is_maximized = OPTION(kGeomIsMaximized);

    // Create the main window.
    frame = wxDynamicCast(xr->LoadFrame(nullptr, "MainFrame"), MainFrame);
    if (!frame) {
        wxLogError(_("Could not create main window"));
        return false;
    }

    // Create() cannot be overridden easily
    if (!frame->BindControls()) {
        return false;
    }

    // Measure the full display area.
    wxRect display_rect;
    for (unsigned int i = 0; i < wxDisplay::GetCount(); i++) {
        display_rect.Union(wxDisplay(i).GetClientArea());
    }

    // Ensure we are not drawing out of bounds.
    if (display_rect.Intersects(client_rect)) {
        frame->SetSize(client_rect);
    }

    if (is_maximized) {
        frame->Maximize();
    }
    if (is_fullscreen && wxGetApp().pending_load != wxEmptyString)
        frame->ShowFullScreen(is_fullscreen);

    frame->Show(true);

#ifndef NO_ONLINEUPDATES
    initAutoupdater();
#endif
    return true;
}

int wxvbamApp::OnRun()
{
    if (console_mode)
    {
        // we could check for our own error codes here...
        return console_status;
    }
    else
    {
        return wxApp::OnRun();
    }
}

// called on --help
bool wxvbamApp::OnCmdLineHelp(wxCmdLineParser& parser)
{
    wxApp::OnCmdLineHelp(parser);
    console_mode = true;
    return true;
}

bool wxvbamApp::OnCmdLineError(wxCmdLineParser& parser)
{
    wxApp::OnCmdLineError(parser);
    console_mode = true;
    console_status = 1;
    return true;
}

void wxvbamApp::OnInitCmdLine(wxCmdLineParser& cl)
{
    wxApp::OnInitCmdLine(cl);
    cl.SetLogo(wxT("VisualBoyAdvance-M\n"));
// 2.9 decided to change all of these from wxChar to char for some
// reason
#if wxCHECK_VERSION(2, 9, 0)
// N_(x) returns x
#define t(x) x
#else
// N_(x) returns wxT(x)
#define t(x) wxT(x)
#endif
    // while I would rather the long options be translated, there is merit
    // to the idea that command-line syntax should not change based on
    // locale
    static wxCmdLineEntryDesc opttab[] = {
        { wxCMD_LINE_OPTION, NULL, t("save-xrc"),
            N_("Save built-in XRC file and exit"),
            wxCMD_LINE_VAL_STRING, 0 },
        { wxCMD_LINE_OPTION, NULL, t("save-over"),
            N_("Save built-in vba-over.ini and exit"),
            wxCMD_LINE_VAL_STRING, 0 },
        { wxCMD_LINE_SWITCH, NULL, t("print-cfg-path"),
            N_("Print configuration path and exit"),
            wxCMD_LINE_VAL_NONE, 0 },
        { wxCMD_LINE_SWITCH, t("f"), t("fullscreen"),
            N_("Start in full-screen mode"),
            wxCMD_LINE_VAL_NONE, 0 },
        { wxCMD_LINE_OPTION, t("c"), t("config"),
            N_("Set a configuration file"),
            wxCMD_LINE_VAL_STRING, 0 },
#if !defined(NO_LINK) && !defined(__WXMSW__)
        { wxCMD_LINE_SWITCH, t("s"), t("delete-shared-state"),
            N_("Delete shared link state first, if it exists"),
            wxCMD_LINE_VAL_NONE, 0 },
#endif
        // stupid wx cmd line parser doesn't support duplicate options
        //    { wxCMD_LINE_OPTION, t("o"),  t("option"),
        //        _("Set configuration option; <opt>=<value> or help for list"),
        { wxCMD_LINE_SWITCH, t("o"), t("list-options"),
            N_("List all settable options and exit"),
            wxCMD_LINE_VAL_NONE, 0 },
        { wxCMD_LINE_PARAM, NULL, NULL,
            N_("ROM file"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
        { wxCMD_LINE_PARAM, NULL, NULL,
            N_("<config>=<value>"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_MULTIPLE | wxCMD_LINE_PARAM_OPTIONAL },
        { wxCMD_LINE_NONE, NULL, NULL, NULL, wxCMD_LINE_VAL_NONE, 0 }
    };
// 2.9 automatically translates desc, but 2.8 doesn't
#if !wxCHECK_VERSION(2, 9, 0)

    for (int i = 0; opttab[i].kind != wxCMD_LINE_NONE; i++)
        opttab[i].description = wxGetTranslation(opttab[i].description);

#endif
    // note that "SetDesc" actually Adds rather than replaces
    // so system standard (port-dependent) options are still included:
    //   -h/--help  --verbose --theme --mode
    cl.SetDesc(opttab);
}

bool wxvbamApp::OnCmdLineParsed(wxCmdLineParser& cl)
{
    if (!wxApp::OnCmdLineParsed(cl))
        return false;

    wxString s;

    if (cl.Found(wxT("save-xrc"), &s)) {
        // This was most likely done on a command line, so use
        // stderr instead of gui for messages
        wxLog::SetActiveTarget(new wxLogStderr);
        wxFileSystem fs;
        wxFSFile* f = fs.OpenFile(wxT("memory:wxvbam.xrs#zip:wxvbam.xrs$wxvbam.xrc"));

        if (!f) {
            wxLogError(_("Configuration / build error: can't find built-in xrc"));
            return false;
        }

        wxFileOutputStream os(s);
        os.Write(*f->GetStream());
        delete f;
        wxString lm;
        lm.Printf(_("Wrote built-in configuration to %s.\n"
                    "To override, remove all but changed root node(s). First found root node of correct name in any ."
                    "xrc or .xrs files in following search path overrides built-in:"),
            s.c_str());
        tack_full_path(lm);
        wxLogMessage(lm);
        console_mode = true;
        return true;
    }

    if (cl.Found(wxT("print-cfg-path"))) {
        // This was most likely done on a command line, so use
        // stderr instead of gui for messages
        wxLog::SetActiveTarget(new wxLogStderr);
        wxString lm(_("Configuration is read from, in order:"));
        tack_full_path(lm);
        wxLogMessage(lm);
        console_mode = true;
        return true;
    }

    if (cl.Found(wxT("save-over"), &s)) {
        // This was most likely done on a command line, so use
        // stderr instead of gui for messages
        wxLog::SetActiveTarget(new wxLogStderr);
        wxFileOutputStream os(s);
        os.Write(builtin_over, sizeof(builtin_over));
        wxString lm;
        lm.Printf(_("Wrote built-in override file to %s\n"
                    "To override, delete all but changed section. First found section is used from search path:"),
            s.c_str());
        wxString oi = wxFileName::GetPathSeparator();
        oi += wxT("vba-over.ini");
        tack_full_path(lm, oi);
        lm.append(_("\n\tbuilt-in"));
        wxLogMessage(lm);
        console_mode = true;
        return true;
    }

    if (cl.Found(wxT("f"))) {
        pending_fullscreen = true;
    }

    if (cl.Found(wxT("o"))) {
        // This was most likely done on a command line, so use
        // stderr instead of gui for messages
        wxLog::SetActiveTarget(new wxLogStderr);

        wxPrintf(_("Options set from the command line are saved if any"
                   " configuration changes are made in the user interface.\n\n"
                   "For flag options, true and false are specified as 1 and 0, respectively.\n\n"));

        for (const config::Option& opt : config::Option::All()) {
            wxPrintf("%s\n", opt.ToHelperString());
        }

        wxPrintf(_("The commands available for the Keyboard/* option are:\n\n"));
        for (int i = 0; i < ncmds; i++)
            wxPrintf(wxT("%s (%s)\n"), cmdtab[i].cmd.c_str(), cmdtab[i].name.c_str());

        console_mode = true;
        return true;
    }

    if (cl.Found(wxT("c"), &s)) {
        wxFileName vbamconf(s);
        if (!vbamconf.FileExists()) {
            wxLogError(_("Configuration file not found."));
            return false;
        }
        config_file_ = s;
    }

#if !defined(NO_LINK) && !defined(__WXMSW__)

    if (cl.Found(wxT("s"))) {
        CleanLocalLink();
    }

#endif
    int nparm = cl.GetParamCount();
    bool complained = false, gotfile = false;

    for (int i = 0; i < nparm; i++) {
        auto p     = cl.GetParam(i);
        auto parts = strutils::split(p, wxT('='));

        if (parts.size() > 1) {
            opt_set(parts[0], parts[1]);

            pending_optset.push_back(p);
        }
        else {
            if (!gotfile) {
                pending_load = p;
                gotfile = true;
            } else {
                if (!complained) {
                    wxFprintf(stderr, _("Bad configuration option or multiple ROM files given:\n"));
                    wxFprintf(stderr, wxT("%s\n"), pending_load.c_str());
                    complained = true;
                }

                wxFprintf(stderr, wxT("%s\n"), p.c_str());
            }
        }
    }

    return true;
}

wxString wxvbamApp::GetConfigDir()
{
    return GetAbsolutePath(wxString((get_xdg_user_config_home() + kDotDir).c_str(), wxConvLibc));
}

wxString wxvbamApp::GetDataDir()
{
    return GetAbsolutePath(wxString((get_xdg_user_data_home() + kDotDir).c_str(), wxConvLibc));
}

wxvbamApp::~wxvbamApp() {
    if (home != NULL)
    {
        free(home);
        home = NULL;
    }
    delete overrides;

#ifndef NO_ONLINEUPDATES
    shutdownAutoupdater();
#endif
}

MainFrame::MainFrame()
    : wxFrame(),
      paused(false),
      menus_opened(0),
      dialog_opened(0),
      focused(false),
#ifndef NO_LINK
      gba_link_observer_(config::OptionID::kGBALinkHost,
                         std::bind(&MainFrame::EnableNetworkMenu, this)),
#endif
      keep_on_top_styler_(this),
      status_bar_observer_(config::OptionID::kGenStatusBar,
                           std::bind(&MainFrame::OnStatusBarChanged, this, std::placeholders::_1)) {
    jpoll = new JoystickPoller();
    this->Connect(wxID_ANY, wxEVT_SHOW, wxShowEventHandler(JoystickPoller::ShowDialog), jpoll, jpoll);
}

MainFrame::~MainFrame() {
#ifndef NO_LINK
    CloseLink();
#endif
}

void MainFrame::OnStatusBarChanged(config::Option* option) {
    if (option->GetBool())
        GetStatusBar()->Show();
    else
        GetStatusBar()->Hide();

    SendSizeEvent();
    panel->AdjustSize(false);
    SendSizeEvent();
}

BEGIN_EVENT_TABLE(MainFrame, wxFrame)
#include "cmd-evtable.h"
EVT_CONTEXT_MENU(MainFrame::OnMenu)
// this is the main window focus?  Or EVT_SET_FOCUS/EVT_KILL_FOCUS?
EVT_ACTIVATE(MainFrame::OnActivate)
// requires DragAcceptFiles(true); even then may not do anything
EVT_DROP_FILES(MainFrame::OnDropFile)

// for window geometry
EVT_MOVE(MainFrame::OnMove)
EVT_MOVE_START(MainFrame::OnMoveStart)
EVT_MOVE_END(MainFrame::OnMoveEnd)
EVT_SIZE(MainFrame::OnSize)

// For tracking menubar state.
EVT_MENU_OPEN(MainFrame::MenuPopped)
EVT_MENU_CLOSE(MainFrame::MenuPopped)
EVT_MENU_HIGHLIGHT_ALL(MainFrame::MenuPopped)

END_EVENT_TABLE()

void MainFrame::OnActivate(wxActivateEvent& event)
{
    focused = event.GetActive();

    if (panel && focused)
        panel->SetFocus();

    if (OPTION(kPrefPauseWhenInactive)) {
        if (panel && focused && !paused) {
            panel->Resume();
        }
        else if (panel && !focused) {
            panel->Pause();
        }
    }
}

void MainFrame::OnDropFile(wxDropFilesEvent& event)
{
    wxString* f = event.GetFiles();
    // ignore all but last
    wxGetApp().pending_load = f[event.GetNumberOfFiles() - 1];
}

void MainFrame::OnMenu(wxContextMenuEvent& event)
{
    if (IsFullScreen() && ctx_menu) {
        wxPoint p(event.GetPosition());
#if 0 // wx actually recommends ignoring the position

        if (p != wxDefaultPosition)
            p = ScreenToClient(p);

#endif
        PopupMenu(ctx_menu, p);
    }
}

void MainFrame::OnMove(wxMoveEvent&) {
    if (!IsFullScreen() && !IsMaximized()) {
        const wxPoint window_pos = GetScreenPosition();
        OPTION(kGeomWindowX) = window_pos.x;
        OPTION(kGeomWindowY) = window_pos.y;
    }
}

// On Windows pause sound when moving and resizing the window to prevent dsound
// from looping.
void MainFrame::OnMoveStart(wxMoveEvent&) {
#ifdef __WXMSW__
    soundPause();
#endif
}

void MainFrame::OnMoveEnd(wxMoveEvent&) {
#ifdef __WXMSW__
    soundResume();
#endif
}

void MainFrame::OnSize(wxSizeEvent& event)
{
    wxFrame::OnSize(event);
    const wxRect window_rect = GetRect();
    const wxPoint window_pos = GetScreenPosition();

    if (!IsFullScreen() && !IsMaximized()) {
        if (window_rect.GetHeight() > 0 && window_rect.GetWidth() > 0) {
            OPTION(kGeomWindowHeight) = window_rect.GetHeight();
            OPTION(kGeomWindowWidth) = window_rect.GetWidth();
        }
        OPTION(kGeomWindowX) = window_pos.x;
        OPTION(kGeomWindowY) = window_pos.y;
    }

    OPTION(kGeomIsMaximized) = IsMaximized();
    OPTION(kGeomFullScreen) = IsFullScreen();
}

int MainFrame::FilterEvent(wxEvent& event) {
    if (menus_opened || dialog_opened) {
        return wxEventFilter::Event_Skip;
    }

    int command = 0;
    if (event.GetEventType() == wxEVT_KEY_DOWN) {
        const wxKeyEvent& key_event = static_cast<wxKeyEvent&>(event);
        command = gopts.shortcuts.CommandForInput(config::UserInput(key_event));
    } else if (event.GetEventType() == wxEVT_JOY) {
        const wxJoyEvent& joy_event = static_cast<wxJoyEvent&>(event);
        if (joy_event.pressed()) {
            // We ignore "button up" events here.
            command = gopts.shortcuts.CommandForInput(config::UserInput(joy_event));
        }
    }

    if (command == 0) {
        return wxEventFilter::Event_Skip;
    }

    wxCommandEvent command_event(wxEVT_COMMAND_MENU_SELECTED, command);
    command_event.SetEventObject(this);
    this->GetEventHandler()->ProcessEvent(command_event);
    return wxEventFilter::Event_Processed;
}

wxString MainFrame::GetGamePath(wxString path)
{
    wxString game_path = path;

    if (game_path.size()) {
        game_path = wxGetApp().GetAbsolutePath(game_path);
    } else {
        game_path = panel->game_dir();
        wxFileName::Mkdir(game_path, 0777, wxPATH_MKDIR_FULL);
    }

    if (!wxFileName::DirExists(game_path))
        game_path = wxFileName::GetCwd();

    if (!wxIsWritable(game_path))
    {
        game_path = wxGetApp().GetAbsolutePath(wxString((get_xdg_user_data_home() + kDotDir).c_str(), wxConvLibc));
        wxFileName::Mkdir(game_path, 0777, wxPATH_MKDIR_FULL);
    }

    return game_path;
}

void MainFrame::SetJoystick()
{
    /* Remove all attached joysticks to avoid errors while
     * destroying and creating the GameArea `panel`. */
    joy.StopPolling();

    if (!emulating)
        return;

    std::set<wxJoystick> needed_joysticks = gopts.shortcuts.Joysticks();
    for (const auto& iter : gopts.game_control_bindings) {
        for (const auto& input_iter : iter.second) {
            needed_joysticks.emplace(input_iter.joystick());
        }
    }
    joy.PollJoysticks(std::move(needed_joysticks));
}

void MainFrame::StopJoyPollTimer()
{
    if (jpoll && jpoll->IsRunning())
        jpoll->Stop();
}

void MainFrame::StartJoyPollTimer()
{
    if (jpoll && !jpoll->IsRunning())
        jpoll->Start();
}

bool MainFrame::IsJoyPollTimerRunning()
{
    return jpoll->IsRunning();
}

wxEvtHandler* MainFrame::GetJoyEventHandler()
{
    auto focused_window = wxWindow::FindFocus();

    if (focused_window)
        return focused_window;

    auto panel = GetPanel();
    if (!panel)
        return nullptr;

    if (OPTION(kUIAllowJoystickBackgroundInput))
        return panel->GetEventHandler();

    return nullptr;
}

void MainFrame::enable_menus()
{
    for (int i = 0; i < ncmds; i++)
        if (cmdtab[i].mask_flags && cmdtab[i].mi)
            cmdtab[i].mi->Enable((cmdtab[i].mask_flags & cmd_enable) != 0);

    if (cmd_enable & CMDEN_SAVST)
        for (int i = 0; i < 10; i++)
            if (loadst_mi[i])
                loadst_mi[i]->Enable(state_ts[i].IsValid());
}

void MainFrame::update_state_ts(bool force)
{
    bool any_states = false;

    for (int i = 0; i < 10; i++) {
        if (force)
            state_ts[i] = wxInvalidDateTime;

        if (panel->game_type() != IMAGE_UNKNOWN) {
            wxString fn;
            fn.Printf(SAVESLOT_FMT, panel->game_name().wc_str(), i + 1);
            wxFileName fp(panel->state_dir(), fn);
            wxDateTime ts; // = wxInvalidDateTime

            if (fp.IsFileReadable()) {
                ts = fp.GetModificationTime();
                any_states = true;
            }

            // if(ts != state_ts[i] || (force && !ts.IsValid())) {
            // to prevent assertions (stupid wx):
            if (ts.IsValid() != state_ts[i].IsValid() || (ts.IsValid() && ts != state_ts[i]) || (force && !ts.IsValid())) {
                // wx has no easy way of doing the -- bit independent
                // of locale
                // so use a real date and substitute all digits
                wxDateTime fts = ts.IsValid() ? ts : wxDateTime::Now();
                wxString df = fts.Format(wxT("0&0 %x %X"));

                if (!ts.IsValid())
                    for (size_t j = 0; j < df.size(); j++)
                        if (wxIsdigit(df[j]))
                            df[j] = wxT('-');

                df[0] = i == 9 ? wxT('1') : wxT(' ');
                df[2] = wxT('0') + (i + 1) % 10;

                if (loadst_mi[i]) {
                    wxString lab = loadst_mi[i]->GetItemLabel();
                    size_t tab = lab.find(wxT('\t')), dflen = df.size();

                    if (tab != wxString::npos)
                        df.append(lab.substr(tab));

                    loadst_mi[i]->SetItemLabel(df);
                    loadst_mi[i]->Enable(ts.IsValid());

                    if (tab != wxString::npos)
                        df.resize(dflen);
                }

                if (savest_mi[i]) {
                    wxString lab = savest_mi[i]->GetItemLabel();
                    size_t tab = lab.find(wxT('\t'));

                    if (tab != wxString::npos)
                        df.append(lab.substr(tab));

                    savest_mi[i]->SetItemLabel(df);
                }
            }

            state_ts[i] = ts;
        }
    }

    int cmd_flg = any_states ? CMDEN_SAVST : 0;

    if ((cmd_enable & CMDEN_SAVST) != cmd_flg) {
        cmd_enable = (cmd_enable & ~CMDEN_SAVST) | cmd_flg;
        enable_menus();
    }
}

int MainFrame::oldest_state_slot()
{
    wxDateTime ot;
    int os = -1;

    for (int i = 0; i < 10; i++) {
        if (!state_ts[i].IsValid())
            return i + 1;

        if (os < 0 || state_ts[i] < ot) {
            os = i;
            ot = state_ts[i];
        }
    }

    return os + 1;
}

int MainFrame::newest_state_slot()
{
    wxDateTime nt;
    int ns = -1;

    for (int i = 0; i < 10; i++) {
        if (!state_ts[i].IsValid())
            continue;

        if (ns < 0 || state_ts[i] > nt) {
            ns = i;
            nt = state_ts[i];
        }
    }

    return ns + 1;
}

void MainFrame::ResetRecentAccelerators() {
    for (int i = wxID_FILE1; i <= wxID_FILE10; i++) {
        wxMenuItem* menu_item = recent->FindItem(i);
        if (!menu_item) {
            break;
        }
        ResetMenuItemAccelerator(menu_item);
    }
}

void MainFrame::ResetMenuAccelerators() {
    for (int i = 0; i < ncmds; i++) {
        if (!cmdtab[i].mi) {
            continue;
        }
        ResetMenuItemAccelerator(cmdtab[i].mi);
    }
    ResetRecentAccelerators();
}

void MainFrame::MenuPopped(wxMenuEvent& evt)
{
    // We consider the menu closed when the main menubar or system menu is closed, not any submenus.
    // On Windows nullptr is the system menu.
    if (evt.GetEventType() == wxEVT_MENU_CLOSE && (evt.GetMenu() == nullptr || evt.GetMenu()->GetMenuBar() == GetMenuBar()))
        SetMenusOpened(false);
    else
        SetMenusOpened(true);

    evt.Skip();
}

// On Windows, opening the menubar will stop the app, but DirectSound will
// loop, so we pause audio here.
void MainFrame::SetMenusOpened(bool state)
{
    menus_opened = state;

#ifdef __WXMSW__
    if (menus_opened)
        soundPause();
    else if (!paused)
        soundResume();
#endif
}

// ShowModal that also disables emulator loop
// uses dialog_opened as a nesting counter
int MainFrame::ShowModal(wxDialog* dlg)
{
    dlg->SetWindowStyle(dlg->GetWindowStyle() | wxCAPTION | wxRESIZE_BORDER);

    if (OPTION(kDispKeepOnTop))
        dlg->SetWindowStyle(dlg->GetWindowStyle() | wxSTAY_ON_TOP);
    else
        dlg->SetWindowStyle(dlg->GetWindowStyle() & ~wxSTAY_ON_TOP);

    CheckPointer(dlg);
    StartModal();
    int ret = dlg->ShowModal();
    StopModal();
    return ret;
}

void MainFrame::StartModal()
{
    // workaround for lack of wxGTK mouse motion events: unblank
    // pointer when dialog popped up
    // it will auto-hide again once game resumes
    panel->ShowPointer();
    //panel->Pause();
    ++dialog_opened;
}

void MainFrame::StopModal()
{
    if (!dialog_opened) // technically an error in the caller
        return;

    --dialog_opened;

    if (!IsPaused())
        panel->Resume();
}

#ifndef NO_LINK

LinkMode MainFrame::GetConfiguredLinkMode()
{
    switch (gopts.gba_link_type) {
    case 0:
        return LINK_DISCONNECTED;
        break;

    case 1:
        if (OPTION(kGBALinkProto))
                return LINK_CABLE_IPC;
        else
            return LINK_CABLE_SOCKET;

        break;

    case 2:
        if (OPTION(kGBALinkProto))
            return LINK_RFU_IPC;
        else
            return LINK_RFU_SOCKET;

        break;

    case 3:
        return LINK_GAMECUBE_DOLPHIN;
        break;

    case 4:
        if (OPTION(kGBALinkProto))
            return LINK_GAMEBOY_IPC;
        else
            return LINK_GAMEBOY_SOCKET;

        break;

    default:
        return LINK_DISCONNECTED;
        break;
    }

    return LINK_DISCONNECTED;
}

#endif  // NO_LINK

void MainFrame::IdentifyRom()
{
    if (!panel->rom_name.empty())
        return;

    panel->rom_name = panel->loaded_game.GetFullName();
    wxString name;
    wxString scene_rls;
    wxString scene_name;
    wxString rom_crc32_str;
    rom_crc32_str.Printf(wxT("%08X"), panel->rom_crc32);

    if (wxGetApp().rom_database_nointro.FileExists()) {
        wxFileInputStream input(wxGetApp().rom_database_nointro.GetFullPath());
        wxTextInputStream text(input, wxT("\x09"), wxConvUTF8);

        while (input.IsOk() && !input.Eof()) {
            wxString line = text.ReadLine();

            if (line.Contains(wxT("<releaseNumber>"))) {
                scene_rls = line.AfterFirst('>').BeforeLast('<');
            }

            if (line.Contains(wxT("romCRC ")) && line.Contains(rom_crc32_str)) {
                panel->rom_scene_rls = scene_rls;
                break;
            }
        }
    }

    if (wxGetApp().rom_database_scene.FileExists()) {
        wxFileInputStream input(wxGetApp().rom_database_scene.GetFullPath());
        wxTextInputStream text(input, wxT("\x09"), wxConvUTF8);

        while (input.IsOk() && !input.Eof()) {
            wxString line = text.ReadLine();

            if (line.StartsWith(wxT("\tname"))) {
                scene_name = line.AfterLast(' ').BeforeLast('"');
            }

            if (line.StartsWith(wxT("\trom")) && line.Contains(wxT("crc ") + rom_crc32_str)) {
                panel->rom_scene_rls_name = scene_name;
                panel->rom_name = scene_name;
                break;
            }
        }
    }

    if (wxGetApp().rom_database.FileExists()) {
        wxFileInputStream input(wxGetApp().rom_database.GetFullPath());
        wxTextInputStream text(input, wxT("\x09"), wxConvUTF8);

        while (input.IsOk() && !input.Eof()) {
            wxString line = text.ReadLine();

            if (line.StartsWith(wxT("\tname"))) {
                name = line.AfterFirst('"').BeforeLast('"');
            }

            if (line.StartsWith(wxT("\trom")) && line.Contains(wxT("crc ") + rom_crc32_str)) {
                panel->rom_name = name;
                break;
            }
        }
    }
}

// global event filter
// apparently required for win32; just setting accel table still misses
// a few keys (e.g. only ctrl-x works for exit, but not esc & ctrl-q;
// ctrl-w does not work for close).  It's possible another entity is
// grabbing those keys, but I can't track it down.
int wxvbamApp::FilterEvent(wxEvent& event)
{
    if (frame)
        return frame->FilterEvent(event);
    return wxApp::FilterEvent(event);
}
