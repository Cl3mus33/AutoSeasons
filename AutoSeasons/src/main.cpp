#include "ASConfig.hpp"
#include "ASLocale.hpp"
#include "AutoSeasonsRunner.hpp"
#include "GUI/LauncherWindow.hpp"
#include "GUI/ProgressWindow.hpp"
#include "common/BethesdaGame.hpp"
#include "util/ExceptionHandler.hpp"

#include <CLI/CLI.hpp>
#include <cpptrace/from_current.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <wx/wx.h>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

using namespace std;

namespace {
auto getExecutablePath() -> filesystem::path
{
    array<wchar_t, MAX_PATH> buffer {};
    if (GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH) == 0) {
        cerr << "Error getting executable path: " << GetLastError() << "\n";
        exit(1);
    }

    filesystem::path outPath = filesystem::path(buffer.data());
    if (filesystem::exists(outPath)) {
        return outPath;
    }

    cerr << "Error getting executable path: path does not exist\n";
    exit(1);
    return {};
}

// True when this process owns its console alone (double-clicked from Explorer, or launched by
// MO2/a similar tool that spawns a fresh console) rather than being run from an existing
// terminal. Used to decide whether to pause before exiting, so error output isn't lost to a
// console window that closes the instant the process ends.
auto isSoleConsoleOwner() -> bool
{
    array<DWORD, 2> processList {};
    const auto count = GetConsoleProcessList(processList.data(), static_cast<DWORD>(processList.size()));
    return count <= 1;
}

void pauseBeforeExit()
{
    cout << "\nPress ENTER to exit...";
    cin.get();
}

void configureDotnetLibDirectory(const filesystem::path& exeDir)
{
    const auto libDir = exeDir / "AutoSeasons_dotnetlib";
    if (!filesystem::exists(libDir)) {
        return;
    }

    if (SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS) == 0) {
        cerr << "Failed to configure DLL search directories.\n";
        exit(1);
    }

    if (AddDllDirectory(libDir.c_str()) == nullptr) {
        cerr << "Failed to add dotnetlib directory to DLL search path.\n";
        exit(1);
    }
}

struct AutoSeasonsCLIArgs {
    int verbosity = 0;
    bool multithreading = true;
    bool shortcut = false;
    ASParams params;
    string gameType = "skyrimse";
};

void addArguments(CLI::App& app, AutoSeasonsCLIArgs& args)
{
    app.add_flag("-v", args.verbosity, "Verbosity level -v for DEBUG data or -vv for TRACE data");
    app.add_flag("--no-multithreading", args.multithreading, "Disable multithreading")->default_val(false);
    app.add_flag("--shortcut", args.shortcut, "Keep AutoSeasons running at the end");

    app.add_option(
           "game-dir", args.params.gameDir, "Path to your game's install directory (or merged mod-manager view)")
        ->required();
    app.add_option("output", args.params.outputDir, "Output directory")->default_str("AutoSeasons_Output");
    app.add_option("--game-type", args.gameType, "skyrimse, skyrimvr, skyrimgog, or enderalse")
        ->default_str("skyrimse");
    app.add_option("--blocklist", args.params.meshBlockList,
                   "Glob patterns (e.g. */interiors/*) for meshes to exclude from seasonal swapping")
        ->delimiter(',');
    app.add_option("--editor-id-blocklist", args.params.seasonLockedEditorIDKeywords,
                   "EditorID keywords (case-insensitive substring match, e.g. coast,river) for records to "
                   "exclude from seasonal swapping entirely")
        ->delimiter(',');
    app.add_option("--override-season-mods", args.params.overrideForeignSeasonMods,
                   "Foreign mod names (comma-separated, e.g. \"Turn of the Seasons\") whose Data/Seasons "
                   "coverage AutoSeasons should override with its own generated textures")
        ->delimiter(',');
    app.add_option("--mod-priority", args.params.foreignSeasonModPriority,
                   "Foreign mod names (comma-separated, lowest priority first) - when two foreign mods "
                   "both cover the same record, the one listed later wins. Per-record-type overrides are "
                   "config-file-only (foreignSeasonModPriorityByType in AutoSeasons_config.json)")
        ->delimiter(',');
    app.add_flag("--remove-grass-in-winter,!--no-remove-grass-in-winter", args.params.removeGrassInWinter,
        "Drop grass slots with no detected winter mesh variant instead of leaving them visible under snow "
        "(default: on)");
    app.add_flag("--dry-run", args.params.dryRun,
                   "Run the full scan and log what would be created/skipped, but write nothing to the output "
                   "directory - useful for iterating on --blocklist/--editor-id-blocklist safely")
        ->default_val(false);
    app.add_option("--esm-mode", args.params.esmMode, "0 = ESM-flag output plugin, 1 = same, 2 = do not ESM-flag")
        ->default_val(0);
}

auto runCLI(int argC, char** argV, const filesystem::path& exePath) -> int
{
    AutoSeasonsCLIArgs args;
    CLI::App app {"AutoSeasons: generates Seasons of Skyrim form-swap data from seasonal texture variants"};
    addArguments(app, args);

    try {
        app.parse(argC, argV);
    } catch (const CLI::ParseError& e) {
        const auto code = app.exit(e);
        if (code != 0 && isSoleConsoleOwner()) {
            pauseBeforeExit();
        }
        return code;
    }

    args.params.gameType = BethesdaGame::getGameTypeFromStr(args.gameType);

    const vector<spdlog::sink_ptr> sinks { make_shared<spdlog::sinks::stdout_color_sink_mt>() };
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    if (args.verbosity >= 1) {
        spdlog::set_level(spdlog::level::debug);
    }
    if (args.verbosity >= 2) {
        spdlog::set_level(spdlog::level::trace);
    }

    CPPTRACE_TRY { AutoSeasonsRunner::run(args.params, exePath, sinks, args.multithreading); }
    CPPTRACE_CATCH(const exception& e)
    {
        ExceptionHandler::setException(e, cpptrace::from_current_exception().to_string());
    }

    int returnCode = 0;
    if (ExceptionHandler::hasException()) {
        ExceptionHandler::throwExceptionOnMainThread();
        returnCode = 1;
    }

    // Always pause on failure when nobody's watching an existing terminal, so double-click/MO2
    // launches don't just flash an error and vanish. --shortcut pauses unconditionally.
    if (args.shortcut || (returnCode != 0 && isSoleConsoleOwner())) {
        pauseBeforeExit();
    }

    return returnCode;
}

auto runGUI(const filesystem::path& exePath) -> int
{
    // This is a console-subsystem executable (so CLI mode has a real stdout), which means a
    // console window flashes up behind the GUI unless we hide it here.
    if (HWND consoleWindow = GetConsoleWindow(); consoleWindow != nullptr) {
        ShowWindow(consoleWindow, SW_HIDE);
    }

    wxApp::SetInstance(new wxApp()); // NOLINT(cppcoreguidelines-owning-memory)
    if (!wxEntryStart(nullptr, nullptr)) {
        cerr << "Failed to initialize wxWidgets.\n";
        return 1;
    }

    auto params = ASConfig::load(exePath);
    if (params.outputDir.empty()) {
        params.outputDir = exePath / "AutoSeasons_Output";
    }

    ASLocale::init(exePath / "AutoSeasons_translations", params.uiLanguage);

    int launcherResult = 0;
    do {
        // Applying the appearance must happen before the window it affects is created (it's a
        // wxApp-level setting, not something that live-updates already-shown windows) - re-applied
        // every loop iteration so a theme change made in the Options tab takes effect on relaunch.
        wxApp::Appearance appearance = wxApp::Appearance::System;
        if (params.uiTheme == "light") {
            appearance = wxApp::Appearance::Light;
        } else if (params.uiTheme == "dark") {
            appearance = wxApp::Appearance::Dark;
        }
        if (wxTheApp->SetAppearance(appearance) != wxApp::AppearanceResult::Ok) {
            // Not fatal - the dialog still opens, just without the requested forced appearance
            // (this can happen if the OS/wxWidgets version combination doesn't support overriding
            // the system-wide light/dark preference per-app). Only a console/debugger diagnostic
            // since no log file exists yet at this point in startup.
            cerr << "Could not apply the requested \"" << params.uiTheme << "\" theme; falling back to system appearance.\n";
        }
        // SetAppearance() alone only covers a few high-level things (e.g. the title bar) -
        // painting individual controls (buttons, list boxes, etc.) in dark colors needs wx's
        // separate MSW-specific "experimental" dark mode support turned on via MSWEnableDarkMode().
        // Deliberately only called for "dark" specifically, with DarkMode_Always: calling it with
        // DarkMode_Auto (follow system) turned out to override SetAppearance(Light) too - once
        // dark-capable rendering is enabled at all, an OS in dark mode wins regardless of what
        // SetAppearance() was told, which broke explicit "Light" whenever the OS itself was dark.
        // Only forcing it on for "dark" keeps "Light" reliably light and "Dark" reliably dark, at
        // the cost of "System" not fully dark-painting every control when the OS is dark (its
        // title bar still follows via SetAppearance(System) above) - picking "Dark" directly covers
        // that case fully.
        if (params.uiTheme == "dark") {
            wxTheApp->MSWEnableDarkMode(wxApp::DarkMode_Always);
        }

        auto* launcher = new LauncherWindow(params, exePath); // NOLINT(cppcoreguidelines-owning-memory)
        launcherResult = launcher->ShowModal();
        if (launcherResult == wxID_OK || launcherResult == LauncherWindow::RESULT_RELAUNCH
            || launcherResult == LauncherWindow::RESULT_RESTART) {
            // Preserve the current (possibly unsaved) field values in memory across the rebuild
            // triggered by a language/theme change, so the relaunched window shows the same values.
            launcher->getParams(params);
        }
        launcher->Destroy();
    } while (launcherResult == LauncherWindow::RESULT_RELAUNCH);

    if (launcherResult == LauncherWindow::RESULT_RESTART) {
        ASConfig::save(exePath, params);

        // Respawn a fresh process so wx's MSW dark mode support starts clean with the new theme -
        // see LauncherWindow::onThemeChanged() for why an in-process relaunch isn't reliable here.
        // wxEntryCleanup() is deliberately deferred until after this attempt (and the message box
        // below) - calling it first would tear down wx before a failure could be reported through
        // it, so a failed respawn would silently close the window with zero explanation, looking
        // exactly like a crash.
        STARTUPINFOW startupInfo {};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo {};
        const auto exeFullPath = (exePath / "AutoSeasons.exe").wstring();
        const auto workingDir = exePath.wstring();
        if (CreateProcessW(exeFullPath.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, workingDir.c_str(),
                &startupInfo, &processInfo)
            != 0) {
            CloseHandle(processInfo.hProcess);
            CloseHandle(processInfo.hThread);
        } else {
            const auto errorCode = GetLastError();
            cerr << "Failed to restart AutoSeasons after the theme change (error " << errorCode
                 << "); please relaunch it manually.\n";
            wxMessageBox(wxString::Format("AutoSeasons couldn't restart itself after the theme change (error %lu). "
                                          "Please relaunch it manually.",
                             errorCode),
                "AutoSeasons", wxOK | wxICON_ERROR);
        }

        wxEntryCleanup();
        return 0;
    }

    if (launcherResult != wxID_OK) {
        wxEntryCleanup();
        return 0;
    }

    ASConfig::save(exePath, params);

    auto* progress = new ProgressWindow(params, exePath); // NOLINT(cppcoreguidelines-owning-memory)
    progress->ShowModal();
    progress->Destroy();

    wxEntryCleanup();
    return 0;
}
}

auto main(int argC, char** argV) -> int
{
    SetConsoleOutputCP(CP_UTF8);
    ExceptionHandler::setMainThread();

    const auto exePath = getExecutablePath().parent_path();
    configureDotnetLibDirectory(exePath);

    if (argC <= 1) {
        return runGUI(exePath);
    }

    return runCLI(argC, argV, exePath);
}
