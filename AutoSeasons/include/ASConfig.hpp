#pragma once

#include "common/BethesdaGame.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief User-configurable parameters for AutoSeasons, persisted as JSON next to the executable.
 */
struct ASParams {
    std::string uiLanguage = "en"; ///< GUI language code (AutoSeasons_translations/<code>.json); English if missing.
    std::string uiTheme = "system"; ///< GUI color theme: "system", "light", or "dark".
    std::filesystem::path gameDir;
    BethesdaGame::GameType gameType = BethesdaGame::GameType::SKYRIM_SE;
    std::filesystem::path outputDir;
    // Glob patterns (PathMatchSpecW syntax) matched against each mesh's own relative disk path -
    // records using a mesh under one of these never get a seasonal duplicate, regardless of
    // whether a season-suffixed texture exists for them. Defaults cover vanilla's interior-only
    // asset trees: the whole dungeon-kit tree (caves/mines/dwemer/nordic/imperial/riften/ship/
    // etc. - all interior, never worldspace-placed), Blackreach's own clutter, DLC01's Blackreach/
    // Forgotten Vale cave-worm plants, any path containing "interior" (city/building interior
    // architecture kits, e.g. "wrinteriors", "solitude\interiors"), and generic terrain meshes.
    std::vector<std::wstring> meshBlockList { L"*\\clutter\\blackreach\\*", L"*\\terrain\\*", L"*\\dungeons\\*",
        L"*\\plants\\cave*\\*", L"*interior*" };
    // Records whose EditorID contains one of these keywords (case-insensitive substring match)
    // are skipped entirely by seasonal variation, regardless of which season-suffixed texture
    // files exist on disk - e.g. "river"/"coast" for objects that are never meant to get a
    // seasonal duplicate at all. User-editable so mod authors can tailor it to their own naming.
    // "cave" is a belt-and-suspenders safety net alongside meshBlockList's own dungeon/interior
    // path patterns - it catches a "Cave..."-named record whose mesh happens to live outside
    // those patterns (e.g. a generic clutter mesh reused inside a cave interior). "dead" covers
    // dead trees/vegetation (e.g. "DeadTree01") - already leafless/lifeless year-round, so a
    // seasonal duplicate would nonsensically bring one back to life for spring/summer.
    std::vector<std::wstring> seasonLockedEditorIDKeywords { L"coast", L"river", L"cave", L"dead" };
    // Foreign mod display names (e.g. "Turn of the Seasons", matched case-insensitively against
    // SeasonPatcher::discoverForeignSeasonMods()'s own modName) whose Data/Seasons coverage should
    // be ignored - AutoSeasons generates its own seasonal duplicate for a record one of these mods
    // covers, exactly as if that mod's ini didn't exist. Empty by default (the normal "always
    // respect another mod's own declarations" behavior); populated via the Launcher's "Manage
    // Season Mod Conflicts" dialog.
    std::vector<std::wstring> overrideForeignSeasonMods;
    // Per-record-type override of overrideForeignSeasonMods (key: "STAT"/"LTEX"/"ACTI"/"FURN"/
    // "MSTT"/"TREE"/"FLOR") - additional mods to override for that record type specifically,
    // without overriding them everywhere. Combined (union) with overrideForeignSeasonMods above,
    // not a replacement for it - e.g. a mod can be left respected for STAT while being overridden
    // for TREE. Populated via the Launcher's "Manage Season Mod Conflicts" dialog.
    std::unordered_map<std::string, std::vector<std::wstring>> overrideForeignSeasonModsByType;
    // Cross-mod priority order for foreign Data/Seasons content: when two or more (non-overridden)
    // foreign mods both declare a swap for the very same base record, the mod later in this list
    // wins in the merged ini. A mod not listed here keeps its previous (arbitrary, directory-scan)
    // position. Empty by default. Populated via the Launcher's "Manage Season Mod Conflicts"
    // dialog, alongside overrideForeignSeasonMods above.
    std::vector<std::wstring> foreignSeasonModPriority;
    // Per-record-type override of foreignSeasonModPriority (key: "STAT"/"LTEX"/"ACTI"/"FURN"/
    // "MSTT"/"TREE"/"FLOR"), used instead of the global list for that type specifically when
    // present and non-empty - e.g. preferring Nature of the Wild Lands over Turn of the Seasons
    // for TREE records while still using the global order for everything else.
    std::unordered_map<std::string, std::vector<std::wstring>> foreignSeasonModPriorityByType;
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
    // If true, run() performs the full scan and seasonal-duplication decision pass (so the log's
    // breakdown/diagnostics are accurate) but writes nothing to outputDir - no previous output is
    // cleared, no plugin/ini/PBR files are written. A per-run choice, not a persisted preference -
    // deliberately never read from or written to AutoSeasons_config.json (see ASConfig::load/
    // save), so it always defaults to false and has to be opted into each time.
    bool dryRun = false;
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
