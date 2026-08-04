#pragma once

#include "PGDirectory.hpp"
#include "pgutil/PGTypes.hpp"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <string>
#include <string_view>
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
     * @struct PBRJsonRuleFile
     * @brief One PBRNIFPatcher json rule, plus the path (relative to the output dir) it should be
     * written to - mirrors the source texture's own subfolder under Data\PBRNIFPatcher\.
     */
    struct PBRJsonRuleFile {
        std::filesystem::path relativePath; ///< e.g. "PBRNIFPatcher/landscape/dirt02_win.json"
        nlohmann::json rule;
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
     * @param[out] outPBRRules One PBRNIFPatcher json rule file per unique seasonal PBR diffuse
     * texture found during the pass, for writePBRJsonRules().
     * @return All base/swap FormID pairs created, across all seasons.
     */
    static auto run(PGDirectory* pgd,
                    const std::vector<std::wstring>& meshBlockList,
                    const std::vector<std::wstring>& seasonLockedEditorIDKeywords,
                    bool removeGrassInWinter,
                    std::vector<PBRJsonRuleFile>& outPBRRules) -> std::vector<SwapEntry>;

    /**
     * @brief Writes one Data/Seasons/AutoSeasons_<SUFFIX>.ini file per season present in
     * `swaps`, in the exact format Seasons of Skyrim's FormSwapMap reads
     * (https://github.com/powerof3/SeasonsOfSkyrim): "0x<LocalFormID>~Plugin.esp|0x<LocalFormID>~Plugin.esp"
     * lines under a "[Statics]" section.
     *
     * @param outputDir The mod output directory (an existing Data/Seasons folder is created under it).
     * @param swaps The swap entries produced by run().
     */
    static void writeIniFiles(const std::filesystem::path& outputDir, const std::vector<SwapEntry>& swaps);

    /**
     * @brief Writes each PBRJsonRuleFile collected by run() to its own file under Data\PBRNIFPatcher\,
     * so a downstream PGPatcher run recognizes and PBR-converts AutoSeasons' seasonal duplicates.
     *
     * @param outputDir The mod output directory.
     * @param rules PBR rule files collected via run()'s outPBRRules parameter.
     */
    static void writePBRJsonRules(const std::filesystem::path& outputDir, const std::vector<PBRJsonRuleFile>& rules);

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
     * @brief Given a shape's diffuse (and optionally normal) texture, builds the seasonal variant
     * of just those two slots for one season - never any other slot. Checks the normal (vanilla)
     * texture folder first; if no seasonal sibling exists there, falls back to checking for a PBR
     * seasonal variant under textures\pbr\ of the same relative path.
     *
     * @param pgd The PGDirectory instance (used to check which texture files actually exist).
     * @param baseSlots The shape's current texture set (PGTypes::TextureSet layout); only index 0
     * (diffuse) and index 1 (normal) are ever read.
     * @param season Season suffix, e.g. "AUT".
     * @param includeParallax LTEX only: also look for a season-suffixed parallax (_p) sibling of
     * the diffuse texture and populate slot 3 with it if found - no fallback to the non-seasonal
     * one, since the base and seasonal diffuse can look different enough that their height maps
     * shouldn't be assumed interchangeable. Needed for TerrainHelper, which reads that slot
     * directly off the winning LTEX's TXST - PGPatcher never touches LTEX records at all, so
     * there's no downstream pass to add it the way there is for STAT. run() only ever passes true
     * here when TerrainHelper.esp is actually in the load order (nothing else ever reads this
     * slot), so callers don't need to re-check that themselves.
     * @return A result with slots 0/1 (and, for LTEX, slot 3) populated, or std::nullopt if no
     * seasonal sibling exists at all (neither vanilla-folder nor PBR-folder), or the base diffuse
     * texture itself already lives under textures\pbr\ (a mesh already PBR-patched in-place - not
     * handled here).
     */
    static auto buildSeasonalSlots(PGDirectory* pgd,
                                   const PGTypes::TextureSet& baseSlots,
                                   std::string_view season,
                                   bool includeParallax) -> std::optional<SeasonalSlotResult>;
};
