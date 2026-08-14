#pragma once

#include "PGDirectory.hpp"
#include "pgutil/PGTypes.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * @class SeasonPatcher
 * @brief Scans the known mesh/texture set for seasonal texture variants (e.g. "dirt02_AUT.dds"
 * next to "dirt02.dds") and creates duplicate STAT/LTEX records pointing at them, for use with
 * the Seasons of Skyrim SKSE plugin's Data/Seasons/*.ini form-swap system. This is the whole of
 * AutoSeasons' patching work - it runs once, after PGDirectory has finished scanning the load
 * order, and needs to know every plugin record using a given mesh (via PGDirectory's cached
 * mesh-use data) rather than operating shape-by-shape within one NIF.
 *
 * Deliberately only ever authors the diffuse + normal slots (vanilla texture set shape) - never
 * parallax/complex-material slots (_p/_m/cubemap) itself. PGPatcher's own downstream pass is
 * responsible for adding those, since it re-scans the merged load order (AutoSeasons.esp
 * included) and applies the same complex-material/parallax detection it already does for every
 * other texture.
 *
 * For LTEX, if any seasonal duplicate ends up with a parallax (_p) slot, this class also patches
 * TerrainHelper.esp's "LandscapeDefault" record (the engine's single, no-LTEX-assigned fallback
 * texture) to have its own Height slot filled in - this is what actually switches on
 * TerrainHelper's extended terrain shader globally, letting it read the parallax slot off every
 * other landscape texture set in the game, not just LandscapeDefault's own. The value used there
 * is always LandscapeDefault's own (non-seasonal) parallax sibling, never a seasonal one - if
 * that non-seasonal sibling doesn't exist on disk, LandscapeDefault is left untouched entirely.
 * This is a true override (same FormKey), so it necessarily makes TerrainHelper.esp a master of
 * the output plugin - unavoidable per the Bethesda plugin format (an override can only resolve if
 * its origin plugin is a master), but well-supported: modern Skyrim SE allows a light/ESM-flagged
 * plugin to have a non-master ESP as a master, and this is exactly what TerrainHelper's own
 * documentation recommends doing. A no-op if TerrainHelper.esp isn't in the load order.
 *
 * PBR is handled similarly but needs one extra step: PBR is a NIF shader-level change (shared by
 * every record using that mesh), and PGPatcher's own PBRNIFPatcher json rules normally only
 * match the non-seasonal texture name. So when a seasonal PBR texture is found under
 * textures\pbr\..., this class also authors a matching PBRNIFPatcher json rule for the
 * season-suffixed name (cloning whatever rule already applies to the base texture, if any) - one
 * json file per texture, mirroring the texture's own subfolder under Data\PBRNIFPatcher\ (e.g.
 * textures\landscape\dirt02_win.dds -> Data\PBRNIFPatcher\landscape\dirt02_win.json) - so a
 * downstream PGPatcher run picks up and converts the seasonal duplicate's mesh exactly like it
 * already does for the base texture. AutoSeasons itself never touches/converts NIF files.
 */
class SeasonPatcher {
public:
    /// @brief Season suffixes matching Seasons of Skyrim's own naming convention exactly.
    static constexpr std::array<std::string_view, 4> SEASONS { "WIN", "SPR", "SUM", "AUT" };

    /**
     * @struct SwapEntry
     * @brief A single base-record to seasonal-duplicate-record pairing, ready to be written to a
     * Seasons of Skyrim form-swap ini.
     */
    struct SwapEntry {
        std::wstring baseModName;
        unsigned int baseFormID = 0;
        std::wstring newModName;
        unsigned int newFormID = 0;
        std::string season;
        std::string recordType; ///< "STAT", "LTEX", "ACTI", "FURN", "MSTT", "TREE", or "FLOR"; selects the ini section written for this entry.
    };

    /**
     * @struct ForeignRawLine
     * @brief One raw swap-pair line relayed verbatim from a foreign mod's own Data/Seasons ini,
     * tagged with which mod it came from - the tag lets writeIniFiles() order same-section lines
     * by the user's chosen cross-mod priority instead of an arbitrary directory-scan order (see
     * run()'s foreignSeasonModPriority/foreignSeasonModPriorityByType parameters).
     */
    struct ForeignRawLine {
        std::wstring modName; ///< The covering ini file's own name, e.g. "Turn of the Seasons".
        std::wstring line; ///< The raw swap-pair line, exactly as read from that mod's own ini.
    };

    /**
     * @struct PBRJsonRuleFile
     * @brief One PBRNIFPatcher json rule, plus the path (relative to the output dir) it should be
     * written to - mirrors the source texture's own subfolder under Data\PBRNIFPatcher\.
     */
    struct PBRJsonRuleFile {
        std::filesystem::path relativePath; ///< e.g. "PBRNIFPatcher/landscape/dirt02_win.json"
        nlohmann::json rule;
    };

    /**
     * @struct ForeignSeasonModInfo
     * @brief One foreign mod detected under Data/Seasons, plus how many record/season coverage
     * entries it declares - for discoverForeignSeasonMods()'s GUI-facing mod list.
     */
    struct ForeignSeasonModInfo {
        std::wstring modName; ///< The covering ini file's own name, e.g. "Turn of the Seasons".
        size_t entryCount = 0; ///< Number of record/season combinations this mod covers.
    };

    /**
     * @brief Scans Data/Seasons for every distinct foreign mod's own season-swap ini(s) and
     * returns one entry per mod (not per file - a mod's WIN/SPR/SUM/AUT inis are merged into one
     * entry), with a count of how many record/season coverage declarations it contributes. Used to
     * populate the GUI's "Manage Season Mod Conflicts" picker before a real run, and reuses the
     * exact same parsing run() itself relies on (see loadForeignSeasonCoverage() in the .cpp), so
     * the list shown always matches what a real generation would actually see.
     *
     * @param dataDir The game's Data directory (a "Seasons" subfolder is read from under it).
     * @return One entry per distinct foreign mod detected, sorted by name.
     */
    static auto discoverForeignSeasonMods(const std::filesystem::path& dataDir) -> std::vector<ForeignSeasonModInfo>;

    /**
     * @struct PBRTextureSetFile
     * @brief One Community Shaders "PBRTextureSets" config (a bare json object, not an array,
     * keyed by the TextureSet record's own EditorID), plus the path it should be written to.
     * Unlike PBRJsonRuleFile/PBRNIFPatcher, this is never consumed by PGPatcher - Community
     * Shaders reads it directly at runtime.
     */
    struct PBRTextureSetFile {
        std::filesystem::path relativePath; ///< e.g. "PBRTextureSets/dirt02_AUT_ltex.json"
        nlohmann::json content;
    };

    /**
     * @brief Runs the seasonal duplication pass over all known meshes.
     *
     * @param pgd The populated PGDirectory instance (must have already scanned meshes/textures).
     * @param meshBlockList Glob patterns (matched via PGDirectory::checkGlobMatchInVector) for meshes to
     * skip during this pass only, e.g. interior meshes that Seasons of Skyrim never swaps.
     * @param seasonLockedEditorIDKeywords Records whose EditorID contains one of these keywords
     * (case-insensitive substring match) are skipped entirely, e.g. "river"/"coast".
     * @param removeGrassInWinter If true, the winter LTEX duplicate's Grasses list is emptied
     * (no grass drawn under snow) instead of inheriting the base record's grass types.
     * @param overrideForeignSeasonMods Foreign mod display names (matched case-insensitively
     * against discoverForeignSeasonMods()'s own modName, e.g. "Turn of the Seasons") whose
     * Data/Seasons coverage should be ignored entirely - AutoSeasons generates its own seasonal
     * duplicate for a record one of these mods covers wherever it has the art, AND that mod's own
     * raw ini line is dropped from the merged AIO ini (so a record AutoSeasons has no art for
     * falls back to no swap at all, rather than silently keeping the overridden mod's own swap).
     * Empty by default, preserving the normal "always respect another mod's own declarations"
     * behavior.
     * @param overrideForeignSeasonModsByType Per-record-type union with overrideForeignSeasonMods
     * (key: "STAT"/"LTEX"/"ACTI"/"FURN"/"MSTT"/"TREE"/"FLOR") - lets a mod be overridden for one
     * record type (e.g. TREE) without being overridden everywhere.
     * @param foreignSeasonModPriority Cross-mod priority order (matched case-insensitively against
     * discoverForeignSeasonMods()'s own modName) used when two or more NON-overridden foreign mods
     * both declare a swap for the very same base record in the same merged ini - the mod later in
     * this list wins (Seasons of Skyrim keeps the last value written for a given key). A mod not
     * present in this list keeps its previously-arbitrary directory-scan position. Empty by
     * default, preserving today's directory-scan order.
     * @param foreignSeasonModPriorityByType Per-record-type override of foreignSeasonModPriority
     * (key: "STAT"/"LTEX"/"ACTI"/"FURN"/"MSTT"/"TREE"/"FLOR"), used instead of the global list for
     * that record type specifically when present and non-empty.
     * @param[out] outPBRRules One PBRNIFPatcher json rule file per unique seasonal PBR diffuse
     * texture found during the pass, for writePBRJsonRules().
     * @param[out] outPBRTextureSets One PBRTextureSets config per seasonal LTEX texture set whose
     * base TextureSet has an existing config to clone, for writePBRTextureSetFiles().
     * @param[out] outForeignRawLinesBySection Every other mod's own Data/Seasons swap-pair lines
     * (verbatim, tagged with their covering mod and already ordered per foreignSeasonModPriority/
     * foreignSeasonModPriorityByType), keyed by "SEASON|RecordType", for writeIniFiles() to fold
     * into AutoSeasons' own merged AIO ini.
     * @param[out] outForeignIniFilenames Every foreign ini file's own name whose content is now
     * folded into outForeignRawLinesBySection, for writeForeignIniOverrides().
     * @param[out] outUnresolvedForeignConflictCount Number of record/season combinations covered
     * by two or more different foreign mods with no priority order set between them (see the
     * "Multiple foreign mods cover the same..." warning this same pass logs) - for callers that
     * want to surface it in a completion summary, e.g. the GUI.
     * @return All base/swap FormID pairs created, across all seasons.
     */
    static auto run(PGDirectory* pgd,
                    const std::vector<std::wstring>& meshBlockList,
                    const std::vector<std::wstring>& seasonLockedEditorIDKeywords,
                    bool removeGrassInWinter,
                    const std::vector<std::wstring>& overrideForeignSeasonMods,
                    const std::unordered_map<std::string, std::vector<std::wstring>>& overrideForeignSeasonModsByType,
                    const std::vector<std::wstring>& foreignSeasonModPriority,
                    const std::unordered_map<std::string, std::vector<std::wstring>>& foreignSeasonModPriorityByType,
                    std::vector<PBRJsonRuleFile>& outPBRRules,
                    std::vector<PBRTextureSetFile>& outPBRTextureSets,
                    std::unordered_map<std::string, std::vector<ForeignRawLine>>& outForeignRawLinesBySection,
                    std::vector<std::filesystem::path>& outForeignIniFilenames,
                    size_t& outUnresolvedForeignConflictCount) -> std::vector<SwapEntry>;

    /**
     * @brief Writes one Data/Seasons/z_AIO_AutoSeasons_<SUFFIX>.ini file per season present in
     * `swaps` or `foreignRawLinesBySection`, in the exact format Seasons of Skyrim's FormSwapMap
     * reads (https://github.com/powerof3/SeasonsOfSkyrim):
     * "0x<LocalFormID>~Plugin.esp|0x<LocalFormID>~Plugin.esp" lines under a "[Statics]" section -
     * a lowercase "z_" prefix so this file is evaluated last (alphabetically) and always wins over
     * any other mod's own ini for a record both cover. This is an "AIO" (all-in-one): besides
     * AutoSeasons' own generated duplicates, it also folds in every other detected mod's own
     * season-swap declarations (see run()'s outForeignRawLinesBySection) - paired with
     * writeForeignIniOverrides() neutralizing those mods' own ini files, this becomes the single
     * authoritative season-swap source for the whole load order instead of leaving Seasons of
     * Skyrim to reconcile several separate files via alphabetical priority.
     *
     * @param outputDir The mod output directory (an existing Data/Seasons folder is created under it).
     * @param swaps The swap entries produced by run().
     * @param foreignRawLinesBySection Foreign ini content collected via run()'s
     * outForeignRawLinesBySection parameter.
     */
    static void writeIniFiles(const std::filesystem::path& outputDir, const std::vector<SwapEntry>& swaps,
        const std::unordered_map<std::string, std::vector<ForeignRawLine>>& foreignRawLinesBySection);

    /**
     * @brief Writes an empty (comment-only) override for each foreign Data/Seasons ini file whose
     * content writeIniFiles() has already folded into AutoSeasons' own merged AIO ini - since
     * AutoSeasons' own output mod is expected to load after every mod it scans, this shadows
     * (without ever touching) the original file at the mod-manager virtual-filesystem level, so
     * its swaps aren't evaluated twice. A no-op for any filename AutoSeasons' own output mod isn't
     * actually positioned to override - Seasons of Skyrim would then read both the original and
     * this empty file, which is harmless (empty + already-covered, so no functional difference)
     * but doesn't achieve full deduplication; there's no way for AutoSeasons itself to verify mod
     * order from here, so this relies on the same "AutoSeasons' output goes last" assumption its
     * PBRNIFPatcher/PBRTextureSets output already depends on.
     *
     * @param outputDir The mod output directory.
     * @param foreignIniFilenames Filenames collected via run()'s outForeignIniFilenames parameter.
     */
    static void writeForeignIniOverrides(
        const std::filesystem::path& outputDir, const std::vector<std::filesystem::path>& foreignIniFilenames);

    /**
     * @brief Writes each PBRJsonRuleFile collected by run() to its own file under Data\PBRNIFPatcher\,
     * so a downstream PGPatcher run recognizes and PBR-converts AutoSeasons' seasonal duplicates.
     *
     * @param outputDir The mod output directory.
     * @param rules PBR rule files collected via run()'s outPBRRules parameter.
     */
    static void writePBRJsonRules(const std::filesystem::path& outputDir, const std::vector<PBRJsonRuleFile>& rules);

    /**
     * @brief Writes each PBRTextureSetFile collected by run() to its own file under
     * Data\PBRTextureSets\, for Community Shaders to read directly at runtime.
     *
     * @param outputDir The mod output directory.
     * @param files PBR texture set files collected via run()'s outPBRTextureSets parameter.
     */
    static void writePBRTextureSetFiles(const std::filesystem::path& outputDir, const std::vector<PBRTextureSetFile>& files);

private:
    /**
     * @struct SeasonalSlotResult
     * @brief The diffuse+normal slots to use for one shape's seasonal duplicate, plus whether this
     * came from a PBR-folder match (in which case seasonDiffusePath is also set, for authoring
     * a PBRNIFPatcher json rule).
     */
    struct SeasonalSlotResult {
        PGTypes::TextureSet slots;
        bool isPBR = false;
        std::wstring seasonDiffusePath; ///< e.g. "textures\landscape\dirt02_aut.dds"; only set when isPBR is true.
    };

    /**
     * @brief Given a shape's current texture set, builds its seasonal variant for one season.
     * Whether this is treated as a PBR material is decided purely by whether a PBRNifPatcher rule
     * exists matching the base diffuse texture's name (see pbrConfigs) - never by whether this
     * specific record's own currently-winning override happens to already point at a
     * textures\pbr\... path, since PGPatcher itself converts by texture name, not by ESP state,
     * and different records referencing the same base texture can have diverged (some rewritten by
     * a PBR-authored replacer mod, some not). If a rule matches, every populated PBR slot
     * (diffuse/normal/RMAOS/height/etc) gets its own seasonal duplicate; otherwise, only
     * diffuse+normal are duplicated (the vanilla-style default).
     *
     * @param pgd The PGDirectory instance (used to check which texture files actually exist).
     * @param baseSlots The shape's current texture set (PGTypes::TextureSet layout).
     * @param season Season suffix, e.g. "AUT".
     * @param includeParallax LTEX only, non-PBR case: also look for a season-suffixed parallax
     * (_p) sibling of the diffuse texture and populate slot 3 with it if found - no fallback to
     * the non-seasonal one, since the base and seasonal diffuse can look different enough that
     * their height maps shouldn't be assumed interchangeable. Needed for TerrainHelper, which
     * reads that slot directly off the winning LTEX's TXST - PGPatcher never touches LTEX records
     * at all, so there's no downstream pass to add it the way there is for STAT. run() only ever
     * passes true here when TerrainHelper.esp is actually in the load order (nothing else ever
     * reads this slot), so callers don't need to re-check that themselves.
     * @param pbrConfigIndex Every loaded PBRNifPatcher rule entry, indexed by texture stem (see
     * buildPBRConfigStemIndex), used to decide whether the base texture is PBR-designated at all.
     * @return A result with the relevant slots populated, or std::nullopt if no seasonal sibling
     * exists at all for this texture (vanilla or PBR).
     */
    static auto buildSeasonalSlots(PGDirectory* pgd,
                                   const PGTypes::TextureSet& baseSlots,
                                   std::string_view season,
                                   bool includeParallax,
                                   const std::unordered_map<std::wstring, nlohmann::json>& pbrConfigIndex)
        -> std::optional<SeasonalSlotResult>;
};
