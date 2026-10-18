#include "ASConfig.hpp"
#include "ASLocale.hpp"
#include "AutoSeasonsRunner.hpp"
#include "GUI/LauncherWindow.hpp"
#include "GUI/ProgressWindow.hpp"
#include "Mo2Launcher.hpp"
#include "common/BethesdaGame.hpp"
#include "util/ExceptionHandler.hpp"
#include "util/StringUtil.hpp"

#include <CLI/CLI.hpp>
#include <cpptrace/from_current.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <wx/wx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <unordered_set>
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
    // Run under a Mod Organizer 2 instance and profile instead of from inside MO2 (see Mo2Launcher)
    string mo2Exe;
    string mo2Instance;
    string mo2Profile;
    // Set on the child run that Mo2Launcher starts inside MO2: where it reports its end and its log
    string statusFile;
    string mirrorLog;
    // Settings for that child run, written by the launcher window (everything that has no command line option)
    string configFile;
};

void addArguments(CLI::App& app, AutoSeasonsCLIArgs& args)
{
    app.add_flag("-v", args.verbosity, "Verbosity level -v for DEBUG data or -vv for TRACE data");
    app.add_flag("--no-multithreading", args.multithreading, "Disable multithreading")->default_val(false);
    app.add_flag("--shortcut", args.shortcut, "Keep AutoSeasons running at the end");

    app.add_option("game-dir", args.params.gameDir,
        "Path to your game's install directory (or merged mod-manager view); not needed with --mo2-instance");
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
    app.add_option("--mo2-instance", args.mo2Instance,
           "Run under this Mod Organizer 2 instance (a global instance's name, or the folder of a portable one), "
           "without launching AutoSeasons from MO2. game-dir and output then default to the instance's game and "
           "to its mods\\AutoSeasons Output folder");
    app.add_option("--mo2-profile", args.mo2Profile, "The MO2 profile to use (default: the instance's last used one)");
    app.add_option("--mo2-exe", args.mo2Exe, "Path to ModOrganizer.exe (default: found from MO2's own settings)");
    app.add_option(Mo2Launcher::statusFileOption, args.statusFile)->group("");
    app.add_option(Mo2Launcher::mirrorLogOption, args.mirrorLog)->group("");
    app.add_option("--config-file", args.configFile)->group("");
}


// The positional arguments and the Mod Organizer options of a command line are dropped; every other option is passed
// on as given to the run inside Mod Organizer.
auto optionsForChildRun(int argC, char** argV) -> vector<string>
{
    static const unordered_set<string> flags { "-v", "-vv", "--no-multithreading", "--shortcut", "--dry-run",
        "--remove-grass-in-winter", "--no-remove-grass-in-winter" };
    static const unordered_set<string> mo2Options { "--mo2-instance", "--mo2-profile", "--mo2-exe" };

    vector<string> options;
    for (int i = 1; i < argC; ++i) {
        const string token = argV[i];
        if (token == "--") {
            break; // the rest are positional arguments
        }
        const auto name = token.substr(0, token.find('='));
        if (mo2Options.contains(name)) {
            if (name == token) {
                ++i; // its value is the next token
            }
        } else if (token.starts_with("-") && token.size() > 1) {
            options.push_back(token);
            if (!flags.contains(name) && name == token && i + 1 < argC) {
                options.emplace_back(argV[++i]);
            }
        }
    }
    return options;
}

auto gameTypeOfInstance(const Mo2Launcher::Instance& instance) -> string
{
    if (instance.gameName.find("VR") != string::npos) {
        return "skyrimvr";
    }
    if (instance.gameName.find("Enderal") != string::npos) {
        return "enderalse";
    }
    return "skyrimse";
}

// Runs the whole generation inside the Mod Organizer 2 instance and profile the arguments name: this process only
// starts Mod Organizer, shows what the run logs and returns its exit code.
auto runThroughMo2(AutoSeasonsCLIArgs& args, CLI::App& app, int argC, char** argV) -> int
{
    Mo2Launcher::Request request;
    request.instance = Mo2Launcher::readInstance(StringUtil::utf8toUTF16(args.mo2Instance));
    request.profile = args.mo2Profile.empty() ? request.instance.selectedProfile : StringUtil::utf8toUTF16(args.mo2Profile);
    if (request.profile.empty() || ranges::find(request.instance.profiles, request.profile) == request.instance.profiles.end()) {
        throw runtime_error("The profile \"" + StringUtil::utf16toUTF8(request.profile) + "\" does not exist in the instance \""
            + StringUtil::utf16toUTF8(request.instance.displayName) + "\"");
    }

    if (!args.mo2Exe.empty()) {
        request.mo2Exe = filesystem::path(StringUtil::utf8toUTF16(args.mo2Exe));
    } else if (const auto found = Mo2Launcher::findMo2Exe()) {
        request.mo2Exe = *found;
    } else {
        throw runtime_error("Cannot find ModOrganizer.exe, pass its path with --mo2-exe");
    }
    if (!filesystem::exists(request.mo2Exe)) {
        throw runtime_error("ModOrganizer.exe does not exist: " + request.mo2Exe.string());
    }

    const auto gameDir = args.params.gameDir.empty() ? request.instance.gameDir : filesystem::absolute(args.params.gameDir);
    const auto outputDir = args.params.outputDir.empty() ? request.instance.modsDir / L"AutoSeasons Output"
                                                          : filesystem::absolute(args.params.outputDir);

    request.childArguments = optionsForChildRun(argC, argV);
    if (app.count("--game-type") == 0) {
        request.childArguments.emplace_back("--game-type");
        request.childArguments.push_back(gameTypeOfInstance(request.instance));
    }
    request.childArguments.emplace_back("--");
    request.childArguments.push_back(StringUtil::utf16toUTF8(gameDir.wstring()));
    request.childArguments.push_back(StringUtil::utf16toUTF8(outputDir.wstring()));
    request.onLogLine = [](const string& line) { cout << line << '\n'; };

    spdlog::info("Running under Mod Organizer: instance \"{}\", profile \"{}\"",
        StringUtil::utf16toUTF8(request.instance.displayName), StringUtil::utf16toUTF8(request.profile));
    spdlog::info("Output folder: {}", outputDir.string());
    return Mo2Launcher::runUnderMo2(request);
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

    const bool isChildRun = !args.statusFile.empty();
    if (!args.configFile.empty()) {
        const bool isDryRun = args.params.dryRun;
        args.params = ASConfig::loadFrom(StringUtil::utf8toUTF16(args.configFile));
        args.params.dryRun = isDryRun;
    }

    const vector<spdlog::sink_ptr> consoleSinks { make_shared<spdlog::sinks::stdout_color_sink_mt>() };
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    if (args.verbosity >= 1) {
        spdlog::set_level(spdlog::level::debug);
    }
    if (args.verbosity >= 2) {
        spdlog::set_level(spdlog::level::trace);
    }

    if (!args.mo2Instance.empty() && !isChildRun && !Mo2Launcher::isRunningUnderMo2()) {
        int mo2Result = 1;
        try {
            mo2Result = runThroughMo2(args, app, argC, argV);
        } catch (const exception& e) {
            spdlog::error("{}", e.what());
        }
        if (mo2Result != 0 && isSoleConsoleOwner()) {
            pauseBeforeExit();
        }
        return mo2Result;
    }

    if (args.params.gameDir.empty()) {
        try {
            if (args.mo2Instance.empty()) {
                throw runtime_error("game-dir is required (or --mo2-instance)");
            }
            args.params.gameDir = Mo2Launcher::readInstance(StringUtil::utf8toUTF16(args.mo2Instance)).gameDir;
        } catch (const exception& e) {
            spdlog::error("{}", e.what());
            return 1;
        }
    }

    if (args.configFile.empty()) {
        args.params.gameType = BethesdaGame::getGameTypeFromStr(args.gameType);
    }

    // The child run inside Mod Organizer reports through files its parent follows: its log, and how it ended.
    vector<spdlog::sink_ptr> sinks = consoleSinks;
    if (!args.mirrorLog.empty()) {
        try {
            sinks.push_back(make_shared<spdlog::sinks::basic_file_sink_mt>(
                StringUtil::utf8toUTF16(args.mirrorLog), true));
            spdlog::flush_every(chrono::milliseconds(300));
        } catch (const exception& e) {
            spdlog::error("Cannot write the log for the program that started this run: {}", e.what());
        }
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
    if (isChildRun) {
        spdlog::default_logger()->flush();
        ofstream(StringUtil::utf8toUTF16(args.statusFile)) << "exit " << returnCode << '\n';
    } else if (args.shortcut || (returnCode != 0 && isSoleConsoleOwner())) {
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

    // The run that AutoSeasons starts inside Mod Organizer takes its command line from a file (Mo2Launcher)
    if (argC == 3 && string(argV[1]) == Mo2Launcher::argumentsFileOption) {
        vector<string> lines;
        try {
            lines = Mo2Launcher::readArgumentsFile(StringUtil::utf8toUTF16(argV[2]));
        } catch (const exception& e) {
            cerr << e.what() << '\n';
            return 1;
        }
        vector<char*> pointers { argV[0] };
        for (auto& line : lines) {
            pointers.push_back(line.data());
        }
        return runCLI(static_cast<int>(pointers.size()), pointers.data(), exePath);
    }

    return runCLI(argC, argV, exePath);
}
