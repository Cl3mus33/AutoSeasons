#pragma once

#include "common/BethesdaGame.hpp"

#include <filesystem>
#include <string>
#include <vector>

/**
 * @brief User-configurable parameters for AutoSeasons, persisted as JSON next to the executable.
 */
struct ASParams {
    std::string uiLanguage = "en"; ///< GUI language code (translations/<code>.json); English if missing.
    std::filesystem::path gameDir;
    BethesdaGame::GameType gameType = BethesdaGame::GameType::SKYRIM_SE;
    std::filesystem::path outputDir;
    std::vector<std::wstring> meshBlockList;
    // Records whose EditorID contains one of these keywords (case-insensitive substring match)
    // are skipped entirely by seasonal variation, regardless of which season-suffixed texture
    // files exist on disk - e.g. "river"/"coast" for objects that are never meant to get a
    // seasonal duplicate at all. User-editable so mod authors can tailor it to their own naming.
    std::vector<std::wstring> seasonLockedEditorIDKeywords { L"coast", L"river" };
    // If true (default), any grass slot on a winter LTEX duplicate that has no detected winter
    // mesh variant of its own is dropped entirely (no grass drawn under snow) rather than
    // inheriting its year-round appearance. No GUI toggle for this - edit this key directly in
    // AutoSeasons_config.json (or pass --no-remove-grass-in-winter on the CLI) if you'd rather
    // keep uncovered grass visible in winter.
    bool removeGrassInWinter = true;
    // 0/1 = ESM-flag output plugin, 2 = do not ESM-flag. Defaults to 2 (never ESM-flag): an
    // ESM-flagged plugin historically could only have other ESM-flagged plugins as masters, which
    // would make depending on a regular ESP (e.g. TerrainHelper.esp) unsafe. ESL-flagging (which
    // has no such restriction) is applied automatically and independently of this setting.
    int esmMode = 2;
};

/**
 * @class ASConfig
 * @brief Loads/saves ASParams to a JSON file next to the executable, so settings persist
 * between runs without the user needing to re-type anything.
 */
class ASConfig {
public:
    static auto load(const std::filesystem::path& exeDir) -> ASParams;
    static void save(const std::filesystem::path& exeDir, const ASParams& params);

private:
    static auto getConfigPath(const std::filesystem::path& exeDir) -> std::filesystem::path;
};
