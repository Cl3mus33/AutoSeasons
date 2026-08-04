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
    app.add_flag("--remove-grass-in-winter,!--no-remove-grass-in-winter", args.params.removeGrassInWinter,
        "Drop grass slots with no detected winter mesh variant instead of leaving them visible under snow "
        "(default: on)");
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
    wxTheApp->SetAppearance(wxApp::Appearance::System);

    auto params = ASConfig::load(exePath);
    if (params.outputDir.empty()) {
        params.outputDir = exePath / "AutoSeasons_Output";
    }

    ASLocale::init(exePath / "translations", params.uiLanguage);

    int launcherResult = 0;
    do {
        auto* launcher = new LauncherWindow(params, exePath); // NOLINT(cppcoreguidelines-owning-memory)
        launcherResult = launcher->ShowModal();
        if (launcherResult == wxID_OK || launcherResult == LauncherWindow::RESULT_RELAUNCH) {
            // Preserve the current (possibly unsaved) field values in memory across the rebuild
            // triggered by a language change, so the relaunched window shows the same values.
            launcher->getParams(params);
        }
        launcher->Destroy();
    } while (launcherResult == LauncherWindow::RESULT_RELAUNCH);

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
