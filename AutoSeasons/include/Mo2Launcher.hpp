#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Runs AutoSeasons under a Mod Organizer 2 instance and profile chosen by the user, without the
 * user having to add it to MO2 as an executable.
 *
 * The run itself still happens inside MO2's virtual file system (it is what presents the merged data folder), so
 * "choosing an instance" means this process relaunches itself with `ModOrganizer.exe -i <instance> -p <profile> run`
 * and follows the child's progress through a log file and a status file it writes.
 */
namespace Mo2Launcher {

/// @brief One Mod Organizer 2 instance, read from its ModOrganizer.ini.
struct Instance {
    /// @brief What `-i` takes: the folder name of a global instance, or the full path of a portable one.
    std::wstring argument;
    /// @brief Name shown to the user.
    std::wstring displayName;
    std::filesystem::path iniDir; /**< Folder that holds ModOrganizer.ini */
    std::filesystem::path baseDir; /**< The instance's base directory (mods, profiles, overwrite, downloads) */
    std::filesystem::path modsDir;
    std::filesystem::path gameDir; /**< The game the instance manages */
    std::string gameName; /**< As MO2 writes it, e.g. "Skyrim Special Edition" */
    std::vector<std::wstring> profiles; /**< Profile folder names, sorted */
    std::wstring selectedProfile; /**< The profile MO2 last used, empty when unknown */
};

/// @brief Every global instance in %LOCALAPPDATA%\ModOrganizer, sorted by name; instances without a readable
/// ModOrganizer.ini are left out.
[[nodiscard]] auto findGlobalInstances() -> std::vector<Instance>;

/**
 * @brief Reads one instance.
 *
 * @param nameOrPath The folder name of a global instance, or the folder of a portable instance (the one that holds
 * ModOrganizer.ini).
 * @throws std::runtime_error if there is no such instance.
 */
[[nodiscard]] auto readInstance(const std::wstring& nameOrPath) -> Instance;

/// @brief Where ModOrganizer.exe is, from MO2's nxmhandler.ini or from a running MO2; nothing when it cannot be
/// told.
[[nodiscard]] auto findMo2Exe() -> std::optional<std::filesystem::path>;

/// @brief Whether this process runs inside MO2's virtual file system.
[[nodiscard]] auto isRunningUnderMo2() -> bool;

/**
 * @brief Parses the lines of an arguments file written by runUnderMo2 (one argument per line, UTF-8).
 *
 * @throws std::runtime_error if the file cannot be read.
 */
[[nodiscard]] auto readArgumentsFile(const std::filesystem::path& file) -> std::vector<std::string>;

/// @brief What to run under MO2.
struct Request {
    std::filesystem::path mo2Exe;
    Instance instance;
    std::wstring profile;
    /// @brief The arguments of the child run, as the command line of AutoSeasons takes them (without the Mo2 options).
    std::vector<std::string> childArguments;
    /// @brief Called with every line the child logs.
    std::function<void(const std::string&)> onLogLine;
    /// @brief Polled while waiting; returning true gives up waiting (the child is not stopped).
    std::function<bool()> shouldCancel;
};

/**
 * @brief Starts `ModOrganizer.exe ... run AutoSeasons.exe` and waits for the child to finish.
 *
 * @return The exit code of the child run (0 on success).
 * @throws std::runtime_error if MO2 cannot be started, or the child never reports back.
 */
[[nodiscard]] auto runUnderMo2(const Request& request) -> int;

/// @brief The option names of the child run: not meant for users.
inline constexpr const char* argumentsFileOption = "--args-file";
inline constexpr const char* statusFileOption = "--status-file";
inline constexpr const char* mirrorLogOption = "--mirror-log";

}
