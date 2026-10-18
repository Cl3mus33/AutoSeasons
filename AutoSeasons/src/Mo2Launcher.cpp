#include "Mo2Launcher.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <windows.h>
#include <tlhelp32.h>

using namespace std;

namespace Mo2Launcher {

namespace {

auto utf8ToWide(const string& text) -> wstring
{
    if (text.empty()) {
        return {};
    }
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

auto wideToUtf8(const wstring& text) -> string
{
    if (text.empty()) {
        return {};
    }
    const auto size
        = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

auto toLower(wstring text) -> wstring
{
    ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

/// @brief Undoes the escaping of Qt's ini writer: `\\` for a backslash and `\xNNNN` for a UTF-16 unit.
auto unescapeIniValue(const wstring& value) -> wstring
{
    wstring result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != L'\\' || i + 1 >= value.size()) {
            result += value[i];
            continue;
        }
        const auto next = value[i + 1];
        if (next == L'\\') {
            result += L'\\';
            ++i;
        } else if (next == L'x') {
            size_t end = i + 2;
            unsigned int code = 0;
            while (end < value.size() && end < i + 6 && iswxdigit(value[end]) != 0) {
                code = code * 16 + static_cast<unsigned int>(value[end] <= L'9' ? value[end] - L'0' : (towlower(value[end]) - L'a' + 10));
                ++end;
            }
            if (end > i + 2) {
                result += static_cast<wchar_t>(code);
                i = end - 1;
            } else {
                result += value[i];
            }
        } else {
            result += value[i];
        }
    }
    return result;
}

/// @brief The values of one ini file, by "section/key".
using IniValues = map<wstring, wstring>;

auto readIni(const filesystem::path& file) -> IniValues
{
    ifstream stream(file, ios::binary);
    if (!stream) {
        throw runtime_error("Cannot read " + file.string());
    }
    stringstream buffer;
    buffer << stream.rdbuf();
    auto text = buffer.str();
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }

    IniValues values;
    wstring section;
    istringstream lines(text);
    string line;
    while (getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line[0] == '[' && line.back() == ']') {
            section = utf8ToWide(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == string::npos) {
            continue;
        }
        auto value = unescapeIniValue(utf8ToWide(line.substr(equals + 1)));
        if (value.starts_with(L"@ByteArray(") && value.ends_with(L")")) {
            value = value.substr(11, value.size() - 12);
        }
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
            value = value.substr(1, value.size() - 2);
        }
        values[section + L"/" + utf8ToWide(line.substr(0, equals))] = value;
    }
    return values;
}

auto valueOf(const IniValues& values, const wstring& key) -> wstring
{
    const auto it = values.find(key);
    return it == values.end() ? wstring() : it->second;
}

auto expandBaseDir(wstring path, const filesystem::path& baseDir) -> filesystem::path
{
    constexpr wstring_view placeholder = L"%BASE_DIR%";
    if (const auto pos = path.find(placeholder); pos != wstring::npos) {
        path.replace(pos, placeholder.size(), baseDir.wstring());
    }
    return filesystem::path(path);
}

auto globalInstancesDir() -> filesystem::path
{
    const auto* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (localAppData == nullptr) {
        return {};
    }
    return filesystem::path(localAppData) / L"ModOrganizer";
}

auto readInstanceFolder(const filesystem::path& iniDir, const wstring& argument, const wstring& displayName) -> Instance
{
    const auto values = readIni(iniDir / L"ModOrganizer.ini");

    Instance instance;
    instance.argument = argument;
    instance.displayName = displayName;
    instance.iniDir = iniDir;
    const auto baseDirText = valueOf(values, L"Settings/base_directory");
    instance.baseDir = baseDirText.empty() ? iniDir : filesystem::path(baseDirText);
    const auto modsDirText = valueOf(values, L"Settings/mod_directory");
    instance.modsDir = modsDirText.empty() ? instance.baseDir / L"mods" : expandBaseDir(modsDirText, instance.baseDir);
    instance.gameDir = filesystem::path(valueOf(values, L"General/gamePath"));
    instance.gameName = wideToUtf8(valueOf(values, L"General/gameName"));
    instance.selectedProfile = valueOf(values, L"General/selected_profile");

    const auto profilesDirText = valueOf(values, L"Settings/profiles_directory");
    const auto profilesDir
        = profilesDirText.empty() ? instance.baseDir / L"profiles" : expandBaseDir(profilesDirText, instance.baseDir);
    error_code ec;
    for (const auto& entry : filesystem::directory_iterator(profilesDir, ec)) {
        if (entry.is_directory(ec) && filesystem::exists(entry.path() / L"modlist.txt", ec)) {
            instance.profiles.push_back(entry.path().filename().wstring());
        }
    }
    ranges::sort(instance.profiles, {}, toLower);
    return instance;
}

/// @brief Quotes one argument for a Windows command line the way CommandLineToArgvW reads it back.
auto quoteArgument(const wstring& argument) -> wstring
{
    if (!argument.empty() && argument.find_first_of(L" \t\"") == wstring::npos) {
        return argument;
    }
    wstring result = L"\"";
    size_t backslashes = 0;
    for (const auto c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
        } else {
            result.append(backslashes, L'\\');
            result += c;
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

auto currentExecutable() -> filesystem::path
{
    wstring buffer(MAX_PATH, L'\0');
    while (true) {
        const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw runtime_error("Cannot find the path of AutoSeasons.exe");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

/// @brief Reads what a growing log file gained and hands out its complete lines.
class LogTail {
public:
    explicit LogTail(filesystem::path file)
        : m_file(std::move(file))
    {
    }

    /// @return Whether the file exists yet.
    auto poll(const function<void(const string&)>& onLine) -> bool
    {
        ifstream stream(m_file, ios::binary);
        if (!stream) {
            return false;
        }
        stream.seekg(0, ios::end);
        const auto size = static_cast<streamoff>(stream.tellg());
        if (size > m_offset) {
            string chunk(static_cast<size_t>(size - m_offset), '\0');
            stream.seekg(m_offset);
            stream.read(chunk.data(), static_cast<streamsize>(chunk.size()));
            chunk.resize(static_cast<size_t>(stream.gcount()));
            m_offset += static_cast<streamoff>(chunk.size());
            m_pending += chunk;
            size_t start = 0;
            for (auto end = m_pending.find('\n', start); end != string::npos; end = m_pending.find('\n', start)) {
                auto line = m_pending.substr(start, end - start);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (onLine) {
                    onLine(line);
                }
                start = end + 1;
            }
            m_pending.erase(0, start);
            m_lastGrowth = chrono::steady_clock::now();
        }
        return true;
    }

    [[nodiscard]] auto sinceLastGrowth() const -> chrono::steady_clock::duration
    {
        return chrono::steady_clock::now() - m_lastGrowth;
    }

private:
    filesystem::path m_file;
    streamoff m_offset = 0;
    string m_pending;
    chrono::steady_clock::time_point m_lastGrowth = chrono::steady_clock::now();
};

auto readStatusFile(const filesystem::path& file) -> optional<int>
{
    ifstream stream(file);
    string word;
    int code = 0;
    if (stream && (stream >> word >> code) && word == "exit") {
        return code;
    }
    return nullopt;
}

}

auto findGlobalInstances() -> vector<Instance>
{
    vector<Instance> instances;
    error_code ec;
    for (const auto& entry : filesystem::directory_iterator(globalInstancesDir(), ec)) {
        if (!entry.is_directory(ec) || !filesystem::exists(entry.path() / L"ModOrganizer.ini", ec)) {
            continue;
        }
        try {
            const auto name = entry.path().filename().wstring();
            instances.push_back(readInstanceFolder(entry.path(), name, name));
        } catch (const exception&) {
            // an unreadable instance cannot be offered
        }
    }
    ranges::sort(instances, {}, [](const Instance& instance) { return toLower(instance.displayName); });
    return instances;
}

auto readInstance(const wstring& nameOrPath) -> Instance
{
    error_code ec;
    const auto globalDir = globalInstancesDir() / nameOrPath;
    if (!nameOrPath.empty() && filesystem::exists(globalDir / L"ModOrganizer.ini", ec)) {
        return readInstanceFolder(globalDir, nameOrPath, nameOrPath);
    }
    const auto portableDir = filesystem::path(nameOrPath);
    if (!nameOrPath.empty() && filesystem::exists(portableDir / L"ModOrganizer.ini", ec)) {
        const auto absolute = filesystem::absolute(portableDir);
        return readInstanceFolder(absolute, absolute.wstring(), absolute.filename().wstring());
    }
    throw runtime_error("There is no Mod Organizer instance \"" + wideToUtf8(nameOrPath)
        + "\" (neither a global instance nor a folder with a ModOrganizer.ini)");
}

auto findMo2Exe() -> optional<filesystem::path>
{
    error_code ec;

    // The executable the nxm handler of a global MO2 install points at.
    const auto handlerIni = globalInstancesDir() / L"nxmhandler.ini";
    if (filesystem::exists(handlerIni, ec)) {
        try {
            for (const auto& [key, value] : readIni(handlerIni)) {
                if (key.ends_with(L"\\executable") || key.ends_with(L"/executable")) {
                    const filesystem::path candidate(value);
                    if (toLower(candidate.filename().wstring()) == L"modorganizer.exe"
                        && filesystem::exists(candidate, ec)) {
                        return candidate;
                    }
                }
            }
        } catch (const exception&) {
            // fall through to the running process
        }
    }

    // A running Mod Organizer.
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return nullopt;
    }
    optional<filesystem::path> found;
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    for (auto more = Process32FirstW(snapshot, &entry); more != 0 && !found; more = Process32NextW(snapshot, &entry)) {
        if (toLower(entry.szExeFile) != L"modorganizer.exe") {
            continue;
        }
        if (const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            process != nullptr) {
            wstring buffer(32768, L'\0');
            auto size = static_cast<DWORD>(buffer.size());
            if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) != 0) {
                buffer.resize(size);
                found = filesystem::path(buffer);
            }
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
    return found;
}

auto isRunningUnderMo2() -> bool { return GetModuleHandleW(L"usvfs_x64.dll") != nullptr; }

auto readArgumentsFile(const filesystem::path& file) -> vector<string>
{
    ifstream stream(file, ios::binary);
    if (!stream) {
        throw runtime_error("Cannot read the arguments file " + file.string());
    }
    vector<string> arguments;
    string line;
    while (getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        arguments.push_back(line);
    }
    return arguments;
}

auto runUnderMo2(const Request& request) -> int
{
    const auto workDir = filesystem::temp_directory_path()
        / (L"AutoSeasons_mo2_" + to_wstring(GetCurrentProcessId()) + L"_" + to_wstring(GetTickCount64()));
    filesystem::create_directories(workDir);
    const auto argumentsFile = workDir / L"arguments.txt";
    const auto statusFile = workDir / L"status.txt";
    const auto logFile = workDir / L"run.log";

    struct Cleanup {
        filesystem::path dir;
        ~Cleanup()
        {
            error_code ec;
            filesystem::remove_all(dir, ec);
        }
    } cleanup { workDir };

    {
        // The report options go before the "--" that starts the positional arguments, if there is one
        auto arguments = request.childArguments;
        const auto separator = ranges::find(arguments, string("--"));
        arguments.insert(separator,
            { statusFileOption, wideToUtf8(statusFile.wstring()), mirrorLogOption, wideToUtf8(logFile.wstring()) });
        ofstream out(argumentsFile, ios::binary);
        for (const auto& argument : arguments) {
            out << argument << '\n';
        }
    }

    // Everything after -a is one string that MO2 hands to the program as its command line.
    const auto childCommandLine
        = wstring(utf8ToWide(argumentsFileOption)) + L" " + quoteArgument(argumentsFile.wstring());
    wstring commandLine = quoteArgument(request.mo2Exe.wstring()) + L" -i " + quoteArgument(request.instance.argument)
        + L" -p " + quoteArgument(request.profile) + L" run " + quoteArgument(currentExecutable().wstring()) + L" -a "
        + quoteArgument(childCommandLine);

    if (request.onLogLine) {
        request.onLogLine("Starting: " + wideToUtf8(commandLine));
    }

    // Mod Organizer's command line does nothing when its process has no console, so it gets this process's one (a
    // hidden one when there is none); its own messages go nowhere.
    if (GetConsoleWindow() == nullptr && AllocConsole() != 0) {
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    SECURITY_ATTRIBUTES inheritable { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    const auto nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startupInfo {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nul;
    startupInfo.hStdOutput = nul;
    startupInfo.hStdError = nul;
    PROCESS_INFORMATION processInfo {};
    const auto mo2Dir = request.mo2Exe.parent_path().wstring();
    const auto isStarted = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
        mo2Dir.c_str(), &startupInfo, &processInfo);
    const auto startError = GetLastError();
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (isStarted == 0) {
        throw runtime_error("Cannot start Mod Organizer (" + request.mo2Exe.string() + "), error "
            + to_string(startError));
    }
    CloseHandle(processInfo.hThread);

    LogTail tail(logFile);
    bool mo2Exited = false;
    chrono::steady_clock::time_point mo2ExitTime;
    while (true) {
        const bool hasLog = tail.poll(request.onLogLine);

        if (const auto code = readStatusFile(statusFile)) {
            tail.poll(request.onLogLine);
            if (!mo2Exited) {
                WaitForSingleObject(processInfo.hProcess, 5000);
            }
            CloseHandle(processInfo.hProcess);
            return *code;
        }

        if (request.shouldCancel && request.shouldCancel()) {
            CloseHandle(processInfo.hProcess);
            throw runtime_error("Stopped waiting. AutoSeasons may still be running inside Mod Organizer.");
        }

        if (!mo2Exited && WaitForSingleObject(processInfo.hProcess, 200) == WAIT_OBJECT_0) {
            mo2Exited = true;
            mo2ExitTime = chrono::steady_clock::now();
        } else if (mo2Exited) {
            this_thread::sleep_for(chrono::milliseconds(200));
        }

        // Mod Organizer is gone: either it forwarded the run to an open Mod Organizer, which carries on without
        // it, or it never ran anything. A run that logs nothing for a long time is given up on.
        if (mo2Exited) {
            const auto waited = chrono::steady_clock::now() - mo2ExitTime;
            if (!hasLog && waited > chrono::seconds(60)) {
                CloseHandle(processInfo.hProcess);
                throw runtime_error("Mod Organizer did not start AutoSeasons. Check that the instance and the "
                                    "profile exist, and that no Mod Organizer is open on another instance.");
            }
            if (hasLog && tail.sinceLastGrowth() > chrono::minutes(15)) {
                CloseHandle(processInfo.hProcess);
                throw runtime_error("AutoSeasons stopped reporting for 15 minutes inside Mod Organizer.");
            }
        }
    }
}

}
