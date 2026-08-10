#include "SeasonPatcher.hpp"

#include "PGGlobals.hpp"
#include "PGMutagenWrapper.hpp"
#include "pgutil/PGNIFUtil.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"
#include "util/TaskTracker.hpp"

#include "NifFile.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using namespace std;

namespace {
constexpr size_t NUM_PLUGIN_TEXTURE_SLOTS = 8;
constexpr size_t DIFFUSE_SLOT = 0;
constexpr size_t NORMAL_SLOT = 1;
constexpr size_t PARALLAX_SLOT = 3;

// A single NIF shape's texture set plus the identity (index + name) needed to author a matching
// plugin AlternateTextures entry: shape order in GetShapes() is the same order the game uses for
// the AlternateTextures Index, and NiShape::name is what a brand-new entry's Name must match.
struct ShapeTextureInfo {
    int shapeIndex = -1;
    wstring shapeName;
    PGTypes::TextureSet slots;
};

// std::byte -> char is a lossless reinterpretation (both are single bytes), not a text-encoding
// conversion - PBR json config files are read/parsed as whatever encoding they're actually in
// (typically UTF-8/ASCII, like any hand-authored PGPatcher rule file).
auto bytesToString(const vector<std::byte>& bytes) -> string
{
    string str;
    ranges::transform(bytes, back_inserter(str), [](std::byte b) { return static_cast<char>(b); });
    return str;
}

// Reads every shape's own embedded texture set directly from the NIF, for records that have no
// AlternateTextures override at all (the case PGDirectory's cached mesh-use data can't answer).
auto getAllShapeSlotsFromMesh(PGDirectory* pgd, const std::filesystem::path& meshPath) -> vector<ShapeTextureInfo>
{
    if (!pgd->isFile(meshPath)) {
        return {};
    }

    nifly::NifFile nif = PGNIFUtil::loadNIFFromBytes(pgd->getFile(meshPath), false);
    const auto shapes = nif.GetShapes();

    vector<ShapeTextureInfo> result;
    result.reserve(shapes.size());
    for (size_t s = 0; s < shapes.size(); s++) {
        ShapeTextureInfo info;
        info.shapeIndex = static_cast<int>(s);
        info.shapeName = StringUtil::utf8toUTF16(shapes[s]->name.get());

        for (uint32_t i = 0; i < NUM_PLUGIN_TEXTURE_SLOTS; i++) {
            string tex;
            nif.GetTextureSlot(shapes[s], tex, i);
            if (!tex.empty()) {
                info.slots.at(i) = StringUtil::utf8toUTF16(tex);
            }
        }

        result.push_back(std::move(info));
    }

    return result;
}

// True if any path component of texPath is "pbr" (case-insensitive), i.e. it already lives under
// textures\pbr\... - a mesh already PBR-patched in-place. Not handled by this class.
auto isUnderPBRFolder(const std::filesystem::path& texPath) -> bool
{
    for (const auto& part : texPath) {
        if (boost::algorithm::to_lower_copy(part.wstring()) == L"pbr") {
            return true;
        }
    }
    return false;
}

// Checks editorID against a user-editable keyword blacklist (ASParams::seasonLockedEditorIDKeywords,
// defaulting to "coast"/"river"/"cave"/"dead" - the first two are a subset of Seasons of Skyrim's
// own automatic winter form-swap generator's EditorID blacklist, GenerateLandTextureSnowVariant,
// which also lists "snow"/"ice"/"winter"/"frozen". Snow/ice/winter/frozen are deliberately NOT
// defaults here since some users still want seasonal duplicates for those (e.g. a dedicated
// "_SUM" variant of a snow-named static); coast/river/cave/dead-named objects, by contrast, are
// never meant to get a seasonal duplicate at all, regardless of which season-suffixed texture
// files happen to exist on disk - "cave" specifically backs up meshBlockList's own dungeon/
// interior path patterns for a "Cave..."-named record whose mesh happens to live outside them,
// and "dead" stops dead trees/vegetation from getting a nonsensical "back to life" seasonal
// variant. Exposed as a GUI/config list (like the mesh blocklist) rather than hardcoded, so mod
// authors can tailor it to their own naming conventions.
auto hasSeasonLockedEditorID(const std::wstring& editorID, const vector<wstring>& keywords) -> bool
{
    if (editorID.empty() || keywords.empty()) {
        return false;
    }

    const auto lowerEditorID = boost::algorithm::to_lower_copy(editorID);
    return ranges::any_of(keywords, [&](const wstring& keyword) {
        return !keyword.empty() && lowerEditorID.find(boost::algorithm::to_lower_copy(keyword)) != wstring::npos;
    });
}

// Maps a vanilla-folder texture path to its PBR-folder equivalent, e.g.
// "textures\landscape\dirt02.dds" -> "textures\pbr\landscape\dirt02.dds".
auto toPBRFolderPath(const std::filesystem::path& vanillaPath) -> std::filesystem::path
{
    auto str = vanillaPath.wstring();
    boost::algorithm::to_lower(str);
    const wstring prefix = L"textures\\";
    if (str.starts_with(prefix)) {
        return filesystem::path(prefix + L"pbr\\" + str.substr(prefix.size()));
    }
    return vanillaPath;
}

// Inverse of toPBRFolderPath: "textures\pbr\landscape\dirt02_aut.dds" -> "textures\landscape\dirt02_aut.dds".
auto toVanillaFolderPath(const std::filesystem::path& pbrPath) -> std::filesystem::path
{
    auto str = pbrPath.wstring();
    boost::algorithm::to_lower(str);
    const wstring prefix = L"textures\\pbr\\";
    if (str.starts_with(prefix)) {
        return filesystem::path(L"textures\\" + str.substr(prefix.size()));
    }
    return pbrPath;
}

// Filename suffix aliases some texture packs use instead of Seasons of Skyrim's own WIN/SPR/SUM/
// AUT convention (e.g. "rock01_winter.dds" rather than "rock01_WIN.dds") - tried in order after
// the canonical suffix, so packs already shipping their own seasonal textures under a different
// naming scheme still get picked up without requiring their author to rename anything. Every
// candidate is still an exact full-filename match against the known base name (never a substring
// search), so a texture like "waterfall01_fall.dds" only matches if it genuinely exists on disk
// with that exact name - this doesn't loosen matching, just widens which suffix spelling counts.
// The season used for OUTPUT (ini section, req.seasonSuffix) always stays the canonical WIN/SPR/
// SUM/AUT code regardless of which alias matched here - that part is fixed by Seasons of Skyrim's
// own ini format, not by the source texture's naming.
auto seasonAliases(std::string_view season) -> std::vector<std::wstring>
{
    // Short two-letter forms some grass mesh packs use (e.g. Cathedral 3D Grass Library's own
    // "_Sn" snow variants, widely depended on by other grass mods) - checked alongside the
    // full-word aliases below, never in place of them.
    if (season == "WIN") {
        return { L"win", L"winter", L"sn" };
    }
    if (season == "SPR") {
        return { L"spr", L"spring", L"sp" };
    }
    if (season == "SUM") {
        return { L"sum", L"summer", L"su" };
    }
    if (season == "AUT") {
        return { L"aut", L"autumn", L"fall", L"au" };
    }
    return { boost::algorithm::to_lower_copy(wstring(season.begin(), season.end())) };
}

// Resolves the season-suffixed sibling of a whole MESH file on disk (as opposed to
// seasonSuffixedSibling(), which resolves a texture sibling) - used for grass, where the seasonal
// variant is an entirely different NIF (baked-in texture and all), not an AlternateTextures
// override. e.g. "meshes\landscape\grass\grass01.nif" + "AUT" ->
// "meshes\landscape\grass\grass01_aut.nif" (or "..._autumn.nif"/"..._fall.nif" - see
// seasonAliases()). Same exact-full-filename-match guarantee as seasonSuffixedSibling().
auto grassMeshSuffixedSibling(PGDirectory* pgd, const std::wstring& meshPath, std::string_view season) -> std::wstring
{
    if (meshPath.empty()) {
        return L"";
    }

    const filesystem::path meshFsPath(meshPath);
    const auto stem = boost::algorithm::to_lower_copy(meshFsPath.stem().wstring());
    const auto parentPath = boost::algorithm::to_lower_copy(meshFsPath.parent_path().wstring());
    const auto extension = boost::algorithm::to_lower_copy(meshFsPath.extension().wstring());

    for (const auto& alias : seasonAliases(season)) {
        auto candidateStr = (parentPath.empty() ? L"" : parentPath + L"\\") + stem + L"_" + alias + extension;
        if (pgd->isFile(filesystem::path(candidateStr))) {
            return candidateStr;
        }
    }
    return L"";
}

// Resolves the season-suffixed sibling of a single texture path on disk, or an empty string if
// none exists. e.g. "landscape\dirt02_n.dds" + "AUT" -> "landscape\dirt02_aut_n.dds" (or
// "landscape\dirt02_autumn_n.dds", "landscape\dirt02_fall_n.dds" - see seasonAliases()).
auto seasonSuffixedSibling(PGDirectory* pgd, const std::wstring& originalPath, PGEnums::TextureSlots slot,
    std::string_view season) -> std::wstring
{
    if (originalPath.empty()) {
        return L"";
    }

    const filesystem::path originalFsPath(originalPath);
    // base is the full path (no extension, no suffix), already lowercased by getTexBase
    const auto base = PGNIFUtil::getTexBase(originalFsPath, slot);

    // whatever text sits between the base name and the extension (e.g. "_n"), preserved as-is.
    // Compare full paths (not just filenames) since base includes the directory too.
    auto pathNoExt = (originalFsPath.parent_path() / originalFsPath.stem()).wstring();
    boost::algorithm::to_lower(pathNoExt);

    wstring suffixText;
    if (pathNoExt.size() >= base.size() && pathNoExt.starts_with(base)) {
        suffixText = pathNoExt.substr(base.size());
    }

    auto extension = originalFsPath.extension().wstring();
    boost::algorithm::to_lower(extension);

    for (const auto& seasonAlias : seasonAliases(season)) {
        auto candidateStr = base + L"_" + seasonAlias + suffixText + extension;
        if (pgd->getTextures().contains(filesystem::path(candidateStr))) {
            return candidateStr;
        }
    }
    return L"";
}

// LTEX-only: checks for a season-suffixed parallax (_p) sibling of the given base diffuse texture
// (e.g. dirt02_aut_p.dds) - no fallback to the plain non-seasonal one, since the base and seasonal
// diffuse can look different enough that their height maps genuinely shouldn't be assumed
// interchangeable; if there's no seasonal height map, this slot is simply left unset for that
// season. TerrainHelper reads this slot directly off the winning LTEX's TXST record; PGPatcher has
// no LTEX-handling code at all (it only ever patches NIF shapes), so there's no downstream pass to
// add this slot for us the way there is for STAT/parallax-on-meshes.
auto findParallaxSlot(PGDirectory* pgd, const std::wstring& baseDiffusePath, std::string_view season) -> std::wstring
{
    if (baseDiffusePath.empty()) {
        return L"";
    }

    const filesystem::path diffusePath(baseDiffusePath);
    const auto diffuseBase = PGNIFUtil::getTexBase(diffusePath, PGEnums::TextureSlots::DIFFUSE);
    auto extension = diffusePath.extension().wstring();
    boost::algorithm::to_lower(extension);
    const auto nonSeasonalParallax = diffuseBase + L"_p" + extension;

    return seasonSuffixedSibling(pgd, nonSeasonalParallax, PGEnums::TextureSlots::PARALLAX, season);
}

// One-time (not per-season) step: having TerrainHelper.esp's "LandscapeDefault" Height slot
// filled in is what switches on its extended terrain shader globally, letting it read the
// parallax slot off every *other* landscape texture set in the game too - only triggered once we
// know it's actually relevant (i.e. some seasonal LTEX duplicate ended up with a parallax slot).
// Only ever uses LandscapeDefault's own (non-seasonal) parallax sibling - never a seasonal
// texture, since LandscapeDefault isn't itself a seasonal record and swapping in a specific
// season's texture there would be wrong. If that non-seasonal sibling doesn't exist on disk,
// LandscapeDefault is left untouched entirely (no fallback). No-op if TerrainHelper.esp isn't in
// the load order (libGetLandscapeDefaultDiffuse then returns empty).
void patchLandscapeDefaultParallax(PGDirectory* pgd, const std::wstring& anyParallaxPath)
{
    if (anyParallaxPath.empty()) {
        return;
    }

    const auto landscapeDefaultDiffuse = PGMutagenWrapper::libGetLandscapeDefaultDiffuse();
    if (landscapeDefaultDiffuse.empty()) {
        return;
    }

    const filesystem::path diffusePath(landscapeDefaultDiffuse);
    const auto diffuseBase = PGNIFUtil::getTexBase(diffusePath, PGEnums::TextureSlots::DIFFUSE);
    auto extension = diffusePath.extension().wstring();
    boost::algorithm::to_lower(extension);
    const auto ownParallax = diffuseBase + L"_p" + extension;

    if (!pgd->getTextures().contains(filesystem::path(ownParallax))) {
        return; // no non-seasonal parallax sibling for LandscapeDefault's own diffuse - leave it alone
    }

    Logger::info(L"LandscapeDefault: patching with parallax texture \"{}\"", ownParallax);
    PGMutagenWrapper::libPatchLandscapeDefaultParallax(ownParallax);
}

// One-time (not per-season) step, independent of patchLandscapeDefaultParallax(): Community
// Shaders reads a TextureSet with EditorID "DefaultPBRLand" directly as its own default
// (no-LTEX-assigned) PBR terrain fallback - a separate convention from TerrainHelper.esp's
// "LandscapeDefault", which only matters for the old vanilla-parallax hack and has nothing to do
// with PBR terrain. Many PBR landscape mods already ship their own fully-authored
// "DefaultPBRLand" record, so this only ever creates one if none exists anywhere in the load
// order - never overrides an existing one. Uses the LTEX whose vanilla-equivalent diffuse matches
// LandscapeDefault's own diffuse (still just used to identify which vanilla texture, e.g. dirt02,
// is "the default") and has itself been PBR-converted; its own base (non-seasonal) slots, never a
// seasonal variant.
void ensureDefaultPBRLand(const std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS>& pbrSlots)
{
    Logger::info(L"Ensuring \"DefaultPBRLand\" exists, using PBR texture \"{}\" if it needs creating",
        pbrSlots.at(DIFFUSE_SLOT));
    PGMutagenWrapper::libEnsureDefaultPBRLand(pbrSlots);
}

// PGMutagenWrapper's plugin-facing slot arrays are 8 elements; PGTypes::TextureSet is 9 (an extra
// internal slot unused at the plugin boundary). These convert between the two at that boundary.
auto toTextureSet(const std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS>& pluginSlots) -> PGTypes::TextureSet
{
    PGTypes::TextureSet result {};
    for (size_t i = 0; i < NUM_PLUGIN_TEXTURE_SLOTS; i++) {
        result.at(i) = pluginSlots.at(i);
    }
    return result;
}

auto toPluginSlots(const PGTypes::TextureSet& textureSet) -> std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS>
{
    std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS> result;
    for (size_t i = 0; i < NUM_PLUGIN_TEXTURE_SLOTS; i++) {
        result.at(i) = textureSet.at(i);
    }
    return result;
}

//
// PBR json rule handling
//

// Loads every entry from every currently-scanned Data\PBRNIFPatcher\*.json config (same
// default+entries merging and "texture" -> "match_diffuse" aliasing PGPatcher's own TruePBR
// patcher does), as a flat list - just enough to look up an existing rule to clone from, not the
// full matching engine PGPatcher itself needs (that's overkill for AutoSeasons' narrower job).
auto loadAllPBRConfigEntries(PGDirectory* pgd) -> vector<nlohmann::json>
{
    vector<nlohmann::json> entries;

    for (const auto& configPath : pgd->getPBRJSONs()) {
        const auto configFileStr = bytesToString(pgd->getFile(configPath));

        try {
            nlohmann::json j = nlohmann::json::parse(configFileStr);
            nlohmann::json jDefaults;
            nlohmann::json jEntries;

            if (j.is_object()) {
                if (!j.contains("default") || !j.contains("entries")) {
                    continue;
                }
                jDefaults = j["default"];
                jEntries = j["entries"];
            } else {
                jDefaults = nlohmann::json::object();
                jEntries = j;
            }

            for (auto& element : jEntries) {
                for (const auto& [key, value] : jDefaults.items()) {
                    if (!element.contains(key)) {
                        element[key] = value;
                    }
                }

                if (element.contains("texture")) {
                    element["match_diffuse"] = element["texture"];
                }

                entries.push_back(element);
            }
        } catch (const nlohmann::json::parse_error& e) {
            Logger::warn(L"Failed to parse PBR config {}: {}", configPath.wstring(), StringUtil::utf8toUTF16(e.what()));
            continue;
        }
    }

    return entries;
}

// Finds an existing PBR rule whose match_diffuse matches the given base (non-seasonal) diffuse
// path, to clone its non-matching attributes (roughness_scale, subsurface, multilayer, etc.) for
// a new seasonal rule. A simple suffix-match linear scan - this list is small (at most a few
// hundred entries for a real modlist) and only walked for confirmed seasonal-PBR candidates.
// PBRNifPatcher rules are matched by bare texture-filename stem in practice (real-world rules
// write "texture"/"match_diffuse" as just e.g. "dirt02", never a full path - PGPatcher itself
// matches by texture name across the whole load order, not by folder). Indexed once per run
// (keyed by that stem) rather than linearly scanned on every lookup: with a large modlist
// shipping thousands of PBR rules, findMatchingPBRConfig() used to be called for every
// shape/season combination across the whole load order, making an O(configs) scan per call a
// severe bottleneck (confirmed: one real modlist had 3500+ rule entries, and generation time
// roughly doubled once this rule-driven PBR detection replaced the old O(1) folder-path check).
auto buildPBRConfigStemIndex(const vector<nlohmann::json>& configs) -> unordered_map<wstring, nlohmann::json>
{
    unordered_map<wstring, nlohmann::json> index;
    for (const auto& cfg : configs) {
        if (!cfg.contains("match_diffuse")) {
            continue;
        }

        auto matchDiffuse = StringUtil::utf8toUTF16(cfg["match_diffuse"].get<string>());
        boost::algorithm::to_lower(matchDiffuse);
        const auto matchStem = filesystem::path(matchDiffuse).filename().replace_extension().wstring();

        if (!matchStem.empty() && !index.contains(matchStem)) {
            index[matchStem] = cfg; // first match wins, same as the original linear scan's order
        }
    }
    return index;
}

auto findMatchingPBRConfig(const unordered_map<wstring, nlohmann::json>& configIndex, const wstring& baseDiffusePath)
    -> optional<nlohmann::json>
{
    const auto stem = boost::algorithm::to_lower_copy(filesystem::path(baseDiffusePath).filename().replace_extension().wstring());
    if (const auto it = configIndex.find(stem); it != configIndex.end()) {
        return it->second;
    }
    return nullopt;
}

// Checks whether ANY mod already ships a PBRNifPatcher rule matching this exact season-suffixed
// texture name (e.g. a texture pack that ships both the seasonal .dds files AND its own complete
// PBRNifPatcher rules for them). If so, AutoSeasons must not author its own rule for the same
// name - PGPatcher matches rules by texture name regardless of which mod supplied them, so a
// second rule would be redundant at best and silently override the mod's carefully-tuned
// roughness/subsurface/etc. settings at worst.
auto hasExistingPBRRuleForName(const unordered_map<wstring, nlohmann::json>& configIndex, const wstring& seasonDiffuseFileName) -> bool
{
    const auto targetStem
        = boost::algorithm::to_lower_copy(filesystem::path(seasonDiffuseFileName).replace_extension().wstring());
    return configIndex.contains(targetStem);
}

//
// Community Shaders "PBRTextureSets" (per-TextureSet-EditorID rendering config - separate from
// PBRNIFPatcher and not consumed by PGPatcher at all, read directly by Community Shaders itself).
//

// Indexed once per run by lowercased filename stem, same rationale as buildPBRConfigStemIndex():
// findPBRTextureSetFile() is called per LTEX per season (up to 4x for the same, season-invariant
// base EditorID alone), so an O(files) scan per call added up on a PBR-texture-set-heavy modlist.
auto buildPBRTextureSetStemIndex(const vector<filesystem::path>& pbrTextureSetFiles) -> unordered_map<wstring, filesystem::path>
{
    unordered_map<wstring, filesystem::path> index;
    for (const auto& path : pbrTextureSetFiles) {
        const auto stem = boost::algorithm::to_lower_copy(path.stem().wstring());
        if (!stem.empty() && !index.contains(stem)) {
            index[stem] = path; // first match wins, same as the original linear scan's order
        }
    }
    return index;
}

// Finds an existing PBRTextureSets json file whose stem matches the given TextureSet EditorID
// (case-insensitive - this is how Community Shaders itself matches them to a TXST record).
auto findPBRTextureSetFile(const unordered_map<wstring, filesystem::path>& pbrTextureSetIndex, const wstring& txstEditorID)
    -> optional<filesystem::path>
{
    if (txstEditorID.empty()) {
        return nullopt;
    }

    if (const auto it = pbrTextureSetIndex.find(boost::algorithm::to_lower_copy(txstEditorID)); it != pbrTextureSetIndex.end()) {
        return it->second;
    }

    return nullopt;
}

// Maps a texture path to the PBRNIFPatcher json file it should live in, mirroring the VANILLA
// texture's own subfolder (PBRNifPatcher's structure always follows where the vanilla-named
// texture lives, since that's what tells PGPatcher which texture to convert - never the pbr\
// subfolder itself): "textures\landscape\dirt02_win.dds" ->
// "PBRNIFPatcher\landscape\dirt02_win.json". Also strips a "pbr\" component if present, since the
// already-pbr branch of buildSeasonalSlots() passes a REAL textures\pbr\... path here (not a
// vanilla-style placeholder) - without this, that case would wrongly create a parallel
// "PBRNIFPatcher\pbr\landscape\..." folder instead of reusing "PBRNIFPatcher\landscape\...".
auto toPBRJsonRelativePath(const std::wstring& texturePath) -> std::filesystem::path
{
    const filesystem::path texPath(texturePath);
    auto folder = texPath.parent_path().wstring();
    boost::algorithm::to_lower(folder);

    const wstring texturesPrefix = L"textures\\";
    if (folder.starts_with(texturesPrefix)) {
        folder = folder.substr(texturesPrefix.size());
    }

    const wstring pbrPrefix = L"pbr\\";
    if (folder.starts_with(pbrPrefix)) {
        folder = folder.substr(pbrPrefix.size());
    }

    return filesystem::path(L"PBRNIFPatcher") / folder / (texPath.stem().wstring() + L".json");
}

// Builds the new seasonal PBR rule: the matched base rule's attributes (roughness_scale,
// subsurface, multilayer, emissive, etc.), if any was found, minus every matching-key field -
// only match_diffuse (set to the season-suffixed name) should identify this new rule.
auto buildSeasonalPBRRule(const optional<nlohmann::json>& baseConfig, const wstring& seasonDiffuseFileName)
    -> nlohmann::json
{
    nlohmann::json rule = baseConfig.has_value() ? *baseConfig : nlohmann::json::object();

    static constexpr array<const char*, 7> MATCH_FIELDS
        = { "match_diffuse", "match_normal", "path_contains", "nif_filter", "rename", "texture", "json" };
    for (const auto* field : MATCH_FIELDS) {
        rule.erase(field);
    }
    // PGPatcher rules may also number extra match conditions "match1".."match9" beyond the named
    // fields above - that numbered range is PGPatcher's own rule-format convention, not ours.
    static constexpr int MAX_PBR_NUMBERED_MATCH_FIELD = 9;
    for (int i = 1; i <= MAX_PBR_NUMBERED_MATCH_FIELD; i++) {
        rule.erase("match" + to_string(i));
    }
    rule.erase("meta_matchedFrom");

    rule["match_diffuse"] = StringUtil::utf16toUTF8(seasonDiffuseFileName);
    return rule;
}

// Resolves the real (non-seasonal) PBR slot paths for a texture from its own matched
// PBRNifPatcher rule, for records whose currently-winning ESP override is still vanilla-pathed
// (i.e. the record itself was never touched by a PBR-authored replacer mod, but the texture it
// uses is still PBR-designated elsewhere in the load order) - the rule is the only place that
// actually records the real PBR file paths in that case. Rule slot keys are PGPatcher's own
// 1-indexed convention (e.g. "slot2" = normal, "slot4" = parallax/height, "slot6" = RMAOS),
// exactly one more than PGEnums::TextureSlots' 0-indexed value for the same slot. Diffuse has no
// key of its own (the rule only ever records it as a match stem) - its real PBR path is derived
// by mirroring the vanilla path under textures\pbr\, matching the PBR pack's own folder
// convention (confirmed against real PBRNifPatcher rules, which store every other slot under the
// same textures\PBR\... prefix).
auto slotsFromPBRRule(const nlohmann::json& rule, const std::wstring& vanillaDiffusePath) -> PGTypes::TextureSet
{
    PGTypes::TextureSet slots {};
    slots.at(DIFFUSE_SLOT) = toPBRFolderPath(filesystem::path(vanillaDiffusePath)).wstring();

    for (size_t i = 1; i < NUM_PLUGIN_TEXTURE_SLOTS; i++) {
        const auto key = "slot" + to_string(i + 1);
        if (rule.contains(key) && rule[key].is_string()) {
            slots.at(i) = StringUtil::utf8toUTF16(rule[key].get<string>());
        }
    }

    return slots;
}

//
// Foreign Data/Seasons ini coverage (avoid redundant duplicates for records another mod already
// covers for a given season, e.g. a texture pack that ships its own hand-authored swap data).
//

// Builds the lookup key used both when indexing a foreign ini's entries and when checking one of
// our own candidate records against that index - keeping construction in one place guarantees the
// two sides can never drift out of sync (e.g. differing case-folding).
auto composeForeignCoverageKey(string_view season, string_view recordType, const wstring& modName,
    unsigned int formID) -> wstring
{
    return wstring(season.begin(), season.end()) + L"|" + wstring(recordType.begin(), recordType.end()) + L"|"
        + boost::algorithm::to_lower_copy(modName) + L"|" + to_wstring(formID);
}

// Same idea as composeForeignCoverageKey(), for mods whose own Data/Seasons ini identifies records
// by EditorID rather than FormID~Plugin (see loadForeignSeasonCoverage()).
auto composeForeignCoverageEditorIDKey(string_view season, string_view recordType, const wstring& editorID) -> wstring
{
    return wstring(season.begin(), season.end()) + L"|" + wstring(recordType.begin(), recordType.end()) + L"|"
        + boost::algorithm::to_lower_copy(editorID);
}

// Resolves a foreign mod's own raw season-swap target string ("0x<FormID>~Plugin.esp" or a bare
// EditorID, exactly as ForeignSeasonCoverage::targetByFormIDKey/targetByEditorIDKey stored it) to
// the LTEX it currently identifies - using the same two conventions loadForeignSeasonCoverage()
// already parses for the base/left side. The two indexes are built once per run from
// PGMutagenWrapper::libEnumerateLandscapeTextures()'s own results (see run()).
auto resolveForeignLTEXTarget(const wstring& rawTarget,
    const unordered_map<wstring, PGMutagenWrapper::LandscapeTextureEntry>& byFormKey,
    const unordered_map<wstring, PGMutagenWrapper::LandscapeTextureEntry>& byEditorID)
    -> optional<PGMutagenWrapper::LandscapeTextureEntry>
{
    if (const auto tildePos = rawTarget.find(L'~'); tildePos != wstring::npos) {
        unsigned int formID = 0;
        try {
            formID = static_cast<unsigned int>(stoul(rawTarget.substr(0, tildePos), nullptr, 16));
        } catch (const exception&) {
            return nullopt;
        }
        const auto key = boost::algorithm::to_lower_copy(rawTarget.substr(tildePos + 1)) + L"|" + to_wstring(formID);
        if (const auto it = byFormKey.find(key); it != byFormKey.end()) {
            return it->second;
        }
        return nullopt;
    }
    if (const auto it = byEditorID.find(boost::algorithm::to_lower_copy(rawTarget)); it != byEditorID.end()) {
        return it->second;
    }
    return nullopt;
}

// Two independent coverage indexes, since foreign inis identify records by either FormID~Plugin
// (Seasons of Skyrim's own convention) or by base/seasonal EditorID pair (some hand-authored mods,
// e.g. Nature of the Wild Lands' "alder_cluster_big01_summer|alder_cluster_big01_winter" - no
// FormID/plugin at all). A record only needs to clear whichever index actually applies to it.
struct ForeignSeasonCoverage {
    unordered_set<wstring> byFormID;
    unordered_set<wstring> byEditorID;

    // Mirrors byFormID/byEditorID exactly (same composed keys) but keeps the raw right-hand
    // ("target") side of the line too, instead of just a boolean - lets a caller find out WHICH
    // record a foreign mod declared as its own season-swap target for a given base, e.g. to check
    // whether that target still uses the base's own unmodified texture (see
    // LTEX::getForeignTarget() callers in run() for why this matters for grass-only mod patches).
    unordered_map<wstring, wstring> targetByFormIDKey;
    unordered_map<wstring, wstring> targetByEditorIDKey;

    // EditorIDs that are themselves already the SEASONAL TARGET side of some foreign mod's own
    // swap declaration (e.g. Nature of the Wild Lands' own "beech_common_big01_autumn", which its
    // "_AUT.ini" already swaps into every autumn from "beech_common_big01_summer") - not season-
    // keyed, since a record that's already a declared seasonal output for ANY season isn't a
    // legitimate base for AutoSeasons to further season-duplicate for ANY other season either.
    // Without this, AutoSeasons would happily generate e.g. a "_WIN" duplicate of an already-
    // autumn tree, on top of the mod's own dedicated winter record - a redundant, nonsensical
    // "winter version of the autumn tree" nothing asked for. Lowercased for case-insensitive match.
    unordered_set<wstring> knownForeignSeasonalTargetsByEditorID;

    // Every raw swap-pair line read from a foreign ini, grouped by "SEASON|RecordType" - kept
    // verbatim (not decomposed) since Seasons of Skyrim already parses both the FormID~Plugin and
    // EditorID-pair syntaxes correctly today, so relaying the exact same text into our own merged
    // AIO ini needs no re-interpretation. Used to fold every other mod's own season-swap
    // declarations into one authoritative AutoSeasons file instead of leaving them scattered
    // across several inis for Seasons of Skyrim's alphabetical-priority system to sort out.
    unordered_map<string, vector<wstring>> rawLinesBySection;

    // Every foreign ini file's own name (e.g. "Nature of the Wild Lands_WIN.ini") whose content is
    // now fully folded into rawLinesBySection - used to write a neutralizing empty override for
    // each one (see writeForeignIniOverrides()), so the same swaps aren't evaluated twice.
    vector<filesystem::path> foreignIniFilenames;

    [[nodiscard]] auto isCovered(string_view season, string_view recordType, const wstring& modName,
        unsigned int formID, const wstring& editorID) const -> bool
    {
        if (byFormID.contains(composeForeignCoverageKey(season, recordType, modName, formID))) {
            return true;
        }
        if (!editorID.empty() && byEditorID.contains(composeForeignCoverageEditorIDKey(season, recordType, editorID))) {
            return true;
        }
        return !editorID.empty()
            && knownForeignSeasonalTargetsByEditorID.contains(boost::algorithm::to_lower_copy(editorID));
    }

    // Mirrors isCovered()'s two-branch lookup, returning the raw target string (either
    // "0x<FormID>~Plugin.esp" or a bare EditorID, exactly as the foreign ini wrote it) instead of
    // just whether a match exists.
    [[nodiscard]] auto getForeignTarget(string_view season, string_view recordType, const wstring& modName,
        unsigned int formID, const wstring& editorID) const -> optional<wstring>
    {
        if (const auto it = targetByFormIDKey.find(composeForeignCoverageKey(season, recordType, modName, formID));
            it != targetByFormIDKey.end()) {
            return it->second;
        }
        if (!editorID.empty()) {
            if (const auto it = targetByEditorIDKey.find(composeForeignCoverageEditorIDKey(season, recordType, editorID));
                it != targetByEditorIDKey.end()) {
                return it->second;
            }
        }
        return nullopt;
    }
};

// Reads every "*_<SUFFIX>.ini" file already present under Data/Seasons (Seasons of Skyrim's own
// filename convention - a case-insensitive WIN/SPR/SUM/AUT suffix is what tells it which season a
// config applies to), skipping our own output (the "z_AutoSeasons_" prefix - regenerated every
// run, not "someone else's" data) and Seasons of Skyrim's own "MainFormSwap_*" auto-generated
// scan cache (not stable, hand-authored data - see the skip below for why), and returns which
// (season, recordType, ...) combinations are
// already covered by another mod's ini - either by FormID~Plugin (Seasons of Skyrim's own
// convention) or by base-record EditorID (some mods, e.g. Nature of the Wild Lands, ship their own
// complete season-swap declarations keyed by EditorID pairs instead, since their winter variant is
// an entirely separate hand-authored record/mesh rather than an AlternateTextures swap). Used so
// run() can skip generating a redundant AutoSeasons duplicate for a record another mod already
// handles for that season - harmless either way since our "z_" prefix makes ours win if both
// exist, but avoids wasting a FormID and producing a visually-identical duplicate record for
// nothing (or, worse, sitting alongside a mod's own purpose-built seasonal mesh as a redundant,
// texture-swapped-only duplicate).
auto loadForeignSeasonCoverage(const filesystem::path& seasonsDir) -> ForeignSeasonCoverage
{
    ForeignSeasonCoverage coverage;
    if (!filesystem::exists(seasonsDir) || !filesystem::is_directory(seasonsDir)) {
        return coverage;
    }

    // Mirrors writeIniFiles()'s own sectionNames map, in reverse.
    static const unordered_map<string, string> sectionToRecordType { { "statics", "STAT" },
        { "landtextures", "LTEX" }, { "activators", "ACTI" }, { "furniture", "FURN" },
        { "movablestatics", "MSTT" }, { "trees", "TREE" }, { "flora", "FLOR" } };

    // A throwing directory_iterator would abort the ENTIRE generation run on a single transient
    // file lock (AV scanner, MO2/Vortex VFS quirk) while listing this directory - not worth
    // losing an otherwise-successful run over, so scan with an error_code and stop iterating
    // (keeping whatever coverage was already gathered) rather than letting it propagate.
    error_code dirIterEc;
    for (auto dirIt = filesystem::directory_iterator(seasonsDir, dirIterEc);
        !dirIterEc && dirIt != filesystem::directory_iterator(); dirIt.increment(dirIterEc)) {
        const auto& dirEntry = *dirIt;
        if (!dirEntry.is_regular_file() || boost::algorithm::to_lower_copy(dirEntry.path().extension().wstring()) != L".ini") {
            continue;
        }

        const auto stem = boost::algorithm::to_lower_copy(dirEntry.path().stem().wstring());
        if (stem.starts_with(L"z_aio_autoseasons_")) {
            continue; // our own output
        }
        if (stem.starts_with(L"mainformswap")) {
            // Seasons of Skyrim's OWN auto-generated cache, rebuilt from scratch at every game
            // launch by scanning the current load order's EditorIDs - not a stable, hand-authored
            // mod ini. Already always loses to any user/mod config regardless of filename (see
            // writeIniFiles()'s "z_" prefix comment), so relaying its content serves no purpose -
            // SoS regenerates it itself from the live load order anyway. Worse, once AutoSeasons
            // has run at least once, this file can end up referencing AutoSeasons.esp's own
            // FormIDs from that PREVIOUS run (SoS's scanner doesn't know or care that they're
            // ours) - FormIDs that no longer mean anything once this run creates a fresh
            // AutoSeasons.esp, so relaying them would pollute our own new AIO ini with dead
            // references to a plugin state that no longer exists.
            continue;
        }

        string_view season;
        if (stem.ends_with(L"_win")) {
            season = "WIN";
        } else if (stem.ends_with(L"_spr")) {
            season = "SPR";
        } else if (stem.ends_with(L"_sum")) {
            season = "SUM";
        } else if (stem.ends_with(L"_aut")) {
            season = "AUT";
        } else {
            continue; // not a season-suffixed ini Seasons of Skyrim would read seasonally
        }

        ifstream file(dirEntry.path());
        if (!file.is_open()) {
            continue;
        }

        coverage.foreignIniFilenames.push_back(dirEntry.path().filename());

        string currentRecordType;
        string line;
        while (getline(file, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            if (line.front() == '[' && line.back() == ']') {
                const auto sectionName = boost::algorithm::to_lower_copy(line.substr(1, line.size() - 2));
                const auto it = sectionToRecordType.find(sectionName);
                currentRecordType = it != sectionToRecordType.end() ? it->second : "";
                continue;
            }

            if (currentRecordType.empty()) {
                continue; // inside an unrecognized/unsupported section
            }

            // Either "0x<HexFormID>~Plugin.esp|0x<HexFormID>~Plugin.esp" (Seasons of Skyrim's own
            // convention) or "<BaseEditorID>|<SeasonalEditorID>" (some mods' own hand-authored
            // format - no FormID/plugin at all). Only the left (base) side matters for coverage,
            // since that's the record another mod is already overriding - but a genuine pair (both
            // sides present) is also relayed verbatim into rawLinesBySection, so it can be folded
            // into our own merged AIO ini exactly as Seasons of Skyrim already reads it today.
            const auto pipePos = line.find('|');
            const auto leftSide = pipePos != string::npos ? line.substr(0, pipePos) : line;
            // Right (target) side, if any - hoisted once so both branches below can capture it
            // for targetByFormIDKey/targetByEditorIDKey, not just the EditorID-only branch (which
            // used to be the only one that looked at it at all, via its own local recomputation).
            const auto rightSide = pipePos != string::npos ? line.substr(pipePos + 1) : string();
            if (pipePos != string::npos) {
                coverage.rawLinesBySection[string(season) + "|" + currentRecordType].push_back(StringUtil::utf8toUTF16(line));
            }

            const auto tildePos = leftSide.find('~');
            if (tildePos == string::npos) {
                // No "~Plugin" suffix - treat the whole left side as a base-record EditorID.
                if (!leftSide.empty()) {
                    const auto editorIDKey
                        = composeForeignCoverageEditorIDKey(season, currentRecordType, StringUtil::utf8toUTF16(leftSide));
                    coverage.byEditorID.insert(editorIDKey);
                    if (!rightSide.empty()) {
                        coverage.targetByEditorIDKey[editorIDKey] = StringUtil::utf8toUTF16(rightSide);
                    }
                }
                // The right side (if present) is the seasonal target itself, e.g. NOTW's own
                // "beech_common_big01_autumn" - record it regardless of season, so it's never
                // treated as a fresh base for some OTHER season later.
                if (!rightSide.empty()) {
                    coverage.knownForeignSeasonalTargetsByEditorID.insert(
                        boost::algorithm::to_lower_copy(StringUtil::utf8toUTF16(rightSide)));
                }
                continue;
            }

            unsigned int formID = 0;
            try {
                formID = static_cast<unsigned int>(stoul(leftSide.substr(0, tildePos), nullptr, 16));
            } catch (const exception&) {
                continue;
            }

            const auto modName = StringUtil::utf8toUTF16(leftSide.substr(tildePos + 1));
            const auto formIDKey = composeForeignCoverageKey(season, currentRecordType, modName, formID);
            coverage.byFormID.insert(formIDKey);
            if (!rightSide.empty()) {
                coverage.targetByFormIDKey[formIDKey] = StringUtil::utf8toUTF16(rightSide);
            }
        }
    }

    if (dirIterEc) {
        Logger::warn(L"Stopped scanning {} for foreign season ini files early ({}) - coverage from any "
                     L"files not yet reached may be incomplete for this run",
            seasonsDir.wstring(), StringUtil::utf8toUTF16(dirIterEc.message()));
    }

    return coverage;
}
}

auto SeasonPatcher::buildSeasonalSlots(PGDirectory* pgd,
                                       const PGTypes::TextureSet& baseSlots,
                                       std::string_view season,
                                       bool includeParallax,
                                       const std::unordered_map<std::wstring, nlohmann::json>& pbrConfigIndex) -> std::optional<SeasonalSlotResult>
{
    if (baseSlots.at(DIFFUSE_SLOT).empty()) {
        // no diffuse texture at all, nothing to key the seasonal lookup off of
        return nullopt;
    }

    // PGPatcher converts by TEXTURE NAME match against its own PBRNifPatcher rules, never by
    // reading AutoSeasons' or any other plugin's ESP data - so whether a given record's own
    // currently-winning AlternateTextures/TXST override happens to already point at a
    // textures\pbr\... path or still shows the original vanilla-style reference is not a reliable
    // signal on its own: some records get rewritten by a PBR-authored replacer mod (ESP already
    // pbr-pathed), others still carry their untouched vanilla reference even though the exact same
    // base texture (e.g. "dirt02") is PBR-designated everywhere else in the load order via a
    // PBRNifPatcher rule. Using ESP state as the signal is what caused two different texture sets
    // (one vanilla-styled, one PBR-styled) to get created for what's really the same material - so
    // the only signal that matters is whether a PBRNifPatcher rule exists for this texture's name:
    // if it does, this material is PBR, exclusively, regardless of which specific path this one
    // record's own override happens to show right now.
    if (const auto matchedRule = findMatchingPBRConfig(pbrConfigIndex, baseSlots.at(DIFFUSE_SLOT)); matchedRule.has_value()) {
        const auto isAlreadyPBRPath = isUnderPBRFolder(filesystem::path(baseSlots.at(DIFFUSE_SLOT)));
        // Trust the ESP's own already-resolved slots when this record's own override already
        // points at them (most reliable - it's exactly what's wired up for this shape). Otherwise
        // the ESP still shows the vanilla-style reference (a different, non-PBR-aware mod placed
        // this instance) - fall back to the matched rule's own slot content, PGPatcher's own
        // record of the real PBR file paths for this texture.
        const auto pbrBaseSlots = isAlreadyPBRPath ? baseSlots : slotsFromPBRRule(*matchedRule, baseSlots.at(DIFFUSE_SLOT));
        if (pbrBaseSlots.at(DIFFUSE_SLOT).empty()) {
            return nullopt; // couldn't resolve a real PBR diffuse path at all - nothing to seasonalize
        }

        // Write the complete seasonal texture set directly here: every populated slot (diffuse,
        // normal, RMAOS, subsurface, etc.) gets its own season-suffixed sibling if one exists, via
        // the same per-slot suffix stripping (PGNIFUtil::getTexBase) diffuse/normal already use -
        // e.g. "dirt02_rmaos.dds" correctly matches "dirt02_AUT_rmaos.dds". isPBR is still set so
        // recordPBRRuleIfNeeded() authors a PBRNifPatcher rule for the season-specific name if one
        // doesn't already exist somewhere in the load order - PGPatcher still needs a matching
        // rule to recognize and shade it, real file or not.
        const auto diffusePBRSeasonal
            = seasonSuffixedSibling(pgd, pbrBaseSlots.at(DIFFUSE_SLOT), PGEnums::TextureSlots::DIFFUSE, season);
        if (diffusePBRSeasonal.empty()) {
            return nullopt; // no seasonal variant at all for this PBR texture set
        }

        SeasonalSlotResult result;
        result.slots = pbrBaseSlots; // start from every resolved slot, then override what has a seasonal sibling
        result.slots.at(DIFFUSE_SLOT) = diffusePBRSeasonal;
        result.isPBR = true;
        result.seasonDiffusePath = diffusePBRSeasonal;

        for (size_t i = 1; i < NUM_PLUGIN_TEXTURE_SLOTS; i++) {
            if (i == PARALLAX_SLOT || pbrBaseSlots.at(i).empty()) {
                continue; // parallax is handled separately below; empty slots have nothing to key off of
            }

            const auto seasonalSlot
                = seasonSuffixedSibling(pgd, pbrBaseSlots.at(i), static_cast<PGEnums::TextureSlots>(i), season);
            if (!seasonalSlot.empty()) {
                result.slots.at(i) = seasonalSlot;
            }
            // else: keep this slot's existing (non-seasonal) value - RMAOS roughness/metalness/AO
            // doesn't become visually wrong just by reusing it across seasons the way a height
            // map genuinely can (see the parallax handling below).
        }

        // Parallax/height: PBR terrain reads this slot natively as part of a normal PBR texture
        // set (displacement) - unlike the vanilla-parallax-hack case below, it needs no
        // TerrainHelper unlock, so this is populated whenever the resolved PBR texture set already
        // has its own parallax slot, regardless of includeParallax/TerrainHelper. A non-empty base
        // slot here is itself the signal this is LTEX/terrain in the first place - STAT/TREE
        // AlternateTextures never carry one. No fallback to the base slot's own value if no
        // seasonal one exists - the base and seasonal diffuse can look different enough that
        // their height maps genuinely shouldn't be assumed interchangeable.
        result.slots.at(PARALLAX_SLOT)
            = pbrBaseSlots.at(PARALLAX_SLOT).empty() ? L"" : findParallaxSlot(pgd, pbrBaseSlots.at(DIFFUSE_SLOT), season);

        return result;
    }

    // Not a PBR-designated texture - plain vanilla-style duplication (covers vanilla, parallax,
    // and complex-material textures alike; PGPatcher's own downstream pass adds parallax/CM slots
    // on top of this if the right _p/_m files exist, via its own filename-based auto-detection -
    // no rule needed for that, unlike full PBR).
    const auto diffuseSeasonal = seasonSuffixedSibling(pgd, baseSlots.at(DIFFUSE_SLOT), PGEnums::TextureSlots::DIFFUSE, season);
    if (diffuseSeasonal.empty()) {
        return nullopt; // no seasonal variant at all
    }

    SeasonalSlotResult result;
    result.slots.at(DIFFUSE_SLOT) = diffuseSeasonal;

    const auto normalSeasonal = seasonSuffixedSibling(pgd, baseSlots.at(NORMAL_SLOT), PGEnums::TextureSlots::NORMAL, season);
    if (!normalSeasonal.empty()) {
        result.slots.at(NORMAL_SLOT) = normalSeasonal;
    } else if (!baseSlots.at(NORMAL_SLOT).empty()) {
        // no seasonal normal map on disk - reuse the original (non-seasonal) one rather than
        // leaving the shape with no normal map at all for this season.
        result.slots.at(NORMAL_SLOT) = baseSlots.at(NORMAL_SLOT);
    }

    if (includeParallax) {
        const auto parallax = findParallaxSlot(pgd, baseSlots.at(DIFFUSE_SLOT), season);
        if (!parallax.empty()) {
            result.slots.at(PARALLAX_SLOT) = parallax;
        }
    }

    return result;
}

auto SeasonPatcher::run(PGDirectory* pgd, const std::vector<std::wstring>& meshBlockList,
    const std::vector<std::wstring>& seasonLockedEditorIDKeywords, bool removeGrassInWinter,
    std::vector<PBRJsonRuleFile>& outPBRRules, std::vector<PBRTextureSetFile>& outPBRTextureSets,
    std::unordered_map<std::string, std::vector<std::wstring>>& outForeignRawLinesBySection,
    std::vector<std::filesystem::path>& outForeignIniFilenames) -> std::vector<SwapEntry>
{
    vector<SwapEntry> allSwaps;
    size_t seasonLockedSkipCount = 0;
    size_t foreignCoverageSkipCount = 0;
    auto foreignCoverage = loadForeignSeasonCoverage(pgd->getDataPath() / "Seasons");
    Logger::info("Foreign season coverage: {} FormID-based entr(y/ies), {} EditorID-based entr(y/ies) from {} ini file(s): {}",
        foreignCoverage.byFormID.size(), foreignCoverage.byEditorID.size(), foreignCoverage.foreignIniFilenames.size(),
        [&] {
            string names;
            for (const auto& name : foreignCoverage.foreignIniFilenames) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += StringUtil::utf16toUTF8(name.wstring());
            }
            return names;
        }());

    // Batch requests per (season, recordType) so we make one Mutagen call per group instead of
    // one per record, and so results (which don't echo record type) can still be attributed
    // correctly when building SwapEntry.
    unordered_map<string, vector<PGMutagenWrapper::SeasonalDuplicateRequest>> requestsByGroup;
    const auto groupKey = [](string_view season, string_view recordType) -> string {
        return string(season) + "|" + string(recordType);
    };

    // Loaded once up front (not lazily) - buildSeasonalSlots() itself now needs this list for
    // every single shape/record to decide whether a texture is PBR-designated at all, not just
    // when authoring a rule file afterward. Indexed by texture stem once here too, rather than
    // linearly scanned on every one of those lookups - see buildPBRConfigStemIndex().
    const auto pbrConfigs = loadAllPBRConfigEntries(pgd);
    const auto pbrConfigIndex = buildPBRConfigStemIndex(pbrConfigs);
    const auto pbrTextureSetIndex = buildPBRTextureSetStemIndex(pgd->getPBRTextureSetJSONs());
    unordered_map<wstring, bool> seenPBRRuleFor; // dedup: one rule (and file) per unique seasonal diffuse texture
    unordered_map<wstring, bool> seenPBRTextureSetFor; // dedup: one PBRTextureSets file per unique season TXST EditorID

    const auto recordPBRRuleIfNeeded
        = [&](const PGTypes::TextureSet& baseSlots, const SeasonalSlotResult& seasonResult) -> void {
        if (!seasonResult.isPBR) {
            return;
        }

        const auto key = boost::algorithm::to_lower_copy(seasonResult.seasonDiffusePath);
        if (seenPBRRuleFor.contains(key)) {
            return;
        }
        seenPBRRuleFor[key] = true;

        const auto seasonDiffuseFileName = filesystem::path(seasonResult.seasonDiffusePath).filename().wstring();

        if (hasExistingPBRRuleForName(pbrConfigIndex, seasonDiffuseFileName)) {
            // Some mod (very likely the one that shipped this season texture in the first place)
            // already has a matching rule - don't author a redundant/conflicting one on top of it.
            return;
        }

        const auto matchedBase = findMatchingPBRConfig(pbrConfigIndex, baseSlots.at(DIFFUSE_SLOT));

        outPBRRules.push_back(PBRJsonRuleFile {
            .relativePath = toPBRJsonRelativePath(seasonResult.seasonDiffusePath),
            .rule = buildSeasonalPBRRule(matchedBase, seasonDiffuseFileName),
        });
    };

    // Dedup: many LTEX records commonly share the same handful of grass types, so this caches
    // one seasonal GRAS duplicate per (base grass, season) - keyed on a lowercased "mod|formID"
    // pair, since grass FormKeyRefs come from Mutagen and modName casing isn't guaranteed
    // consistent. A cached nullopt means "already checked, no seasonal mesh sibling exists" (also
    // worth caching, so a grass type with no seasonal variant isn't re-probed for every LTEX that
    // uses it).
    unordered_map<wstring, optional<PGMutagenWrapper::FormKeyRef>> grassDuplicateCache;
    size_t seasonalGrassSwapCount = 0;
    size_t seasonalGrassReuseCount = 0;

    const auto getOrCreateSeasonalGrass
        = [&](const PGMutagenWrapper::FormKeyRef& baseGrass, string_view season) -> optional<PGMutagenWrapper::FormKeyRef> {
        const auto cacheKey = boost::algorithm::to_lower_copy(baseGrass.modName) + L"|" + to_wstring(baseGrass.formID)
            + L"|" + wstring(season.begin(), season.end());
        if (const auto it = grassDuplicateCache.find(cacheKey); it != grassDuplicateCache.end()) {
            return it->second;
        }

        // Prefer a mod author's own dedicated seasonal grass record, if one already exists (e.g.
        // "AegopodiumDrJ_WIN") - creating our own duplicate on top would be redundant at best and,
        // since two different records would then exist for "winter Aegopodium", confusing at
        // worst. Tried before the mesh-based fallback below, using the same season-alias spelling
        // variants (win/winter/sn, etc) mesh matching already accepts.
        if (const auto baseEditorID = PGMutagenWrapper::libGetGrassEditorID(baseGrass.modName, baseGrass.formID);
            !baseEditorID.empty()) {
            for (const auto& alias : seasonAliases(season)) {
                if (const auto existing = PGMutagenWrapper::libFindGrassByEditorID(baseEditorID + L"_" + alias);
                    existing.has_value()) {
                    grassDuplicateCache[cacheKey] = existing;
                    seasonalGrassReuseCount++;
                    return existing;
                }
            }
        }

        const auto meshPath = PGMutagenWrapper::libGetGrassMeshPath(baseGrass.modName, baseGrass.formID);
        const auto seasonalMesh = grassMeshSuffixedSibling(pgd, meshPath, season);
        if (seasonalMesh.empty()) {
            grassDuplicateCache[cacheKey] = nullopt;
            return nullopt;
        }

        const auto result
            = PGMutagenWrapper::libDuplicateGrass(baseGrass.modName, baseGrass.formID, seasonalMesh, string(season));
        if (result.newFormID == 0) {
            grassDuplicateCache[cacheKey] = nullopt;
            return nullopt;
        }

        const PGMutagenWrapper::FormKeyRef newRef { .modName = result.newModName, .formID = result.newFormID };
        grassDuplicateCache[cacheKey] = newRef;
        seasonalGrassSwapCount++;
        return newRef;
    };

    // Discovered entirely from plugin data (PGPlugin::getAllModelUses(), backed by C#'s ModelUses
    // dictionary built from every model-referencing record's own Model.File field) rather than
    // from PGDirectory's own disk-based mesh scan - a record with its own explicit
    // AlternateTextures override carries everything needed for that shape regardless of whether
    // its mesh can be found anywhere in the merged Data view (confirmed real-world gap: a PBR/CM
    // patcher may only output meshes it actually converts, leaving an untouched vanilla mesh's
    // resolution in the merged view to chance). Grouped by mesh path here so the per-mesh
    // defaultShapes cache below still only reads each mesh's contents at most once - same
    // grouping convention as ltexByFormKey/ltexByEditorID below (to_lower_copy on the key).
    unordered_map<wstring, vector<pair<PGMeshPermutationTracker::FormKey, PGPlugin::MeshUseAttributes>>> modelUsesByMesh;
    for (auto& [meshFile, formKey, attrs] : PGPlugin::getAllModelUses()) {
        modelUsesByMesh[boost::algorithm::to_lower_copy(meshFile)].emplace_back(std::move(formKey), std::move(attrs));
    }

    // Drives the GUI progress gauge with real "[N%]" figures during this loop (previously just an
    // indeterminate pulse for the whole "Generating seasonal variation" phase, which - unlike the
    // earlier NIF-loading phase - could run long enough on a big load order to look stalled).
    TaskTracker meshTaskTracker("Generating seasonal variation", modelUsesByMesh.size());

    for (const auto& [meshPathKey, meshUses] : modelUsesByMesh) {
        const filesystem::path meshPath(meshPathKey);

        // Reports exactly one completed job per outer-loop iteration regardless of which of the
        // several continue points below is taken - a destructor fires on every scope exit, so
        // this can't be missed the way a manually-placed completeJob() call before each continue
        // could be.
        struct ProgressGuard {
            TaskTracker& tracker;
            ~ProgressGuard() { tracker.completeJob(TaskTracker::Result::SUCCESS); }
        } progressGuard { meshTaskTracker };

        if (!meshBlockList.empty() && PGDirectory::checkGlobMatchInVector(meshPath.wstring(), meshBlockList)) {
            continue; // e.g. interior meshes, which Seasons of Skyrim never swaps
        }

        optional<vector<ShapeTextureInfo>> defaultShapes; // lazily loaded from the NIF, at most once per mesh;
                                                           // getAllShapeSlotsFromMesh() safely no-ops (empty
                                                           // result) if the mesh can't be found on disk

        for (const auto& [formKey, attrs] : meshUses) {
            // Seasons of Skyrim's FormSwapMap supports these six model-referencing record types
            // (plus VisualEffects/RFCT, not handled here - RFCT records don't carry a Model of
            // their own, they point at a separate ArtObject, so they don't fit this same
            // Model+AlternateTextures duplication approach). Also already filtered server-side in
            // C# (EnumerateAllModelUses' SupportedSeasonRecordTypes) but re-checked here since
            // this same MeshUseAttributes type/loop is shared with getModelUses()'s unfiltered
            // per-mesh results elsewhere.
            static constexpr array<PGPlugin::ModelRecordType, 6> SUPPORTED_RECORD_TYPES {
                PGPlugin::ModelRecordType::STATIC_OBJECT,
                PGPlugin::ModelRecordType::ACTIVATOR,
                PGPlugin::ModelRecordType::FURNITURE,
                PGPlugin::ModelRecordType::MOVEABLE_STATIC,
                PGPlugin::ModelRecordType::TREE,
                PGPlugin::ModelRecordType::FLORA,
            };
            if (ranges::find(SUPPORTED_RECORD_TYPES, attrs.recType) == SUPPORTED_RECORD_TYPES.end()) {
                continue;
            }

            const auto recTypeStr
                = string(EnumStringHelper::stringFromEnum(attrs.recType, PGPlugin::MODEL_RECORD_TYPE_TABLE, ""));

            if (attrs.isIgnored || attrs.isDummyUse) {
                continue;
            }

            if (hasSeasonLockedEditorID(attrs.editorID, seasonLockedEditorIDKeywords)) {
                seasonLockedSkipCount++;
                continue;
            }

            // One base entry per shape that needs its own AlternateTextures override: the record's
            // existing overrides (keyed by their real plugin Index, name left empty since we're
            // matching an existing entry, not authoring a new one), PLUS every shape the NIF itself
            // has that ISN'T already in that override list - a record's AlternateTextures only ever
            // covers the shapes some mod specifically wanted to override; any other shape still
            // uses the NIF's own embedded (baked-in) textures and would otherwise never be
            // considered for seasonal duplication at all, even when a valid seasonal sibling exists
            // on disk for it. Those NIF-read entries need index + name since we may have to author
            // brand-new AlternateTextures entries for them.
            vector<ShapeTextureInfo> baseShapes;
            unordered_set<int> shapeIndicesWithOverride;
            if (!attrs.alternateTextures.empty()) {
                baseShapes.reserve(attrs.alternateTextures.size());
                for (const auto& [shapeIndex, textureSet] : attrs.alternateTextures) {
                    baseShapes.push_back(ShapeTextureInfo {
                        .shapeIndex = static_cast<int>(shapeIndex),
                        .shapeName = L"",
                        .slots = textureSet,
                    });
                    shapeIndicesWithOverride.insert(static_cast<int>(shapeIndex));
                }
            }

            if (!defaultShapes.has_value()) {
                defaultShapes = getAllShapeSlotsFromMesh(pgd, meshPath);
            }
            for (const auto& shapeFromMesh : *defaultShapes) {
                if (!shapeIndicesWithOverride.contains(shapeFromMesh.shapeIndex)) {
                    baseShapes.push_back(shapeFromMesh);
                }
            }

            if (baseShapes.empty()) {
                continue;
            }

            for (const auto season : SEASONS) {
                if (foreignCoverage.isCovered(season, recTypeStr, formKey.modKey, formKey.formID, attrs.editorID)) {
                    foreignCoverageSkipCount++;
                    continue; // another mod's Data/Seasons ini already covers this record for this season
                }

                vector<PGMutagenWrapper::SeasonalTextureOverride> overrides;

                for (const auto& baseShape : baseShapes) {
                    const auto seasonResult = buildSeasonalSlots(pgd, baseShape.slots, season, false, pbrConfigIndex);
                    if (!seasonResult.has_value()) {
                        continue; // this shape has no seasonal sibling for this season
                    }

                    recordPBRRuleIfNeeded(baseShape.slots, *seasonResult);

                    PGMutagenWrapper::SeasonalTextureOverride override;
                    override.shapeIndex = baseShape.shapeIndex;
                    override.shapeName = baseShape.shapeName;
                    override.slots = toPluginSlots(seasonResult->slots);

                    overrides.push_back(std::move(override));
                }

                if (overrides.empty()) {
                    continue; // no shape on this record has a seasonal variant for this season
                }

                PGMutagenWrapper::SeasonalDuplicateRequest req;
                req.modName = formKey.modKey;
                req.formID = formKey.formID;
                req.type = recTypeStr;
                req.seasonSuffix = string(season);
                req.overrides = std::move(overrides);

                requestsByGroup[groupKey(season, recTypeStr)].push_back(std::move(req));
            }
        }
    }

    // LTEX (landscape textures) - a standalone ESP record scan, no mesh involved. No shape/index
    // concept, so this always sends exactly one override with shapeIndex/shapeName unused.
    //
    // Only bother looking for a seasonal parallax (_p) sibling at all if TerrainHelper.esp is
    // actually in the load order - without it, nothing in the engine ever reads a LTEX TXST's
    // Height slot, so populating one would just be dead data. Presence is inferred from whether a
    // "LandscapeDefault" record exists at all (same check patchLandscapeDefaultParallax() already
    // relies on for its own no-op case below).
    const auto landscapeDefaultDiffuse = PGMutagenWrapper::libGetLandscapeDefaultDiffuse();
    const bool terrainHelperPresent = !landscapeDefaultDiffuse.empty();
    const auto landscapeDefaultVanillaDiffuse = boost::algorithm::to_lower_copy(landscapeDefaultDiffuse);

    wstring anyParallaxFound; // first seasonal parallax texture encountered, for LandscapeDefault
    std::optional<std::array<std::wstring, NUM_PLUGIN_TEXTURE_SLOTS>> defaultPBRLandSlots;

    const auto allLandscapeTextures = PGMutagenWrapper::libEnumerateLandscapeTextures();

    // Indexed once here (not scanned per lookup) so a foreign mod's own declared season-swap
    // target (e.g. a grass mod's "LTundra01SPR") can be resolved back to its own current texture
    // and Grasses list - see the foreignCoverage.getForeignTarget() use below.
    unordered_map<wstring, PGMutagenWrapper::LandscapeTextureEntry> ltexByEditorID;
    unordered_map<wstring, PGMutagenWrapper::LandscapeTextureEntry> ltexByFormKey;
    for (const auto& e : allLandscapeTextures) {
        if (!e.editorID.empty()) {
            ltexByEditorID[boost::algorithm::to_lower_copy(e.editorID)] = e;
        }
        ltexByFormKey[boost::algorithm::to_lower_copy(e.modName) + L"|" + to_wstring(e.formID)] = e;
    }

    size_t foreignRetargetCount = 0;
    for (const auto& entry : allLandscapeTextures) {
        if (entry.slots.at(DIFFUSE_SLOT).empty()) {
            continue;
        }

        if (hasSeasonLockedEditorID(entry.editorID, seasonLockedEditorIDKeywords)) {
            seasonLockedSkipCount++;
            continue;
        }

        // Is this LTEX the vanilla default terrain texture (same vanilla-equivalent path as
        // LandscapeDefault's own diffuse), and is it PBR-designated at all? Checked the same
        // rule-driven way buildSeasonalSlots() decides PBR-ness generally - a PBRNifPatcher rule
        // matching the texture's name, not just "is the ESP already pbr-pathed" (which alone would
        // miss a generic PBR texture pack that ships the files/rule for dirt02 under
        // textures\pbr\... without any mod having rewritten this specific LTEX's own
        // AlternateTextures/TXST to point there). LandscapeDefault itself is only used here to
        // identify which vanilla texture counts as "the default" - not patched for the PBR case
        // at all. Capture the full base slot set once, for ensureDefaultPBRLand() after the loop.
        if (!defaultPBRLandSlots.has_value() && !landscapeDefaultVanillaDiffuse.empty()) {
            const auto entryIsAlreadyPBRPath = isUnderPBRFolder(filesystem::path(entry.slots.at(DIFFUSE_SLOT)));
            const auto entryVanillaDiffuse = entryIsAlreadyPBRPath
                ? toVanillaFolderPath(filesystem::path(entry.slots.at(DIFFUSE_SLOT))).wstring()
                : entry.slots.at(DIFFUSE_SLOT);

            if (boost::algorithm::to_lower_copy(entryVanillaDiffuse) == landscapeDefaultVanillaDiffuse) {
                if (const auto matchedRule = findMatchingPBRConfig(pbrConfigIndex, entry.slots.at(DIFFUSE_SLOT));
                    matchedRule.has_value()) {
                    defaultPBRLandSlots = entryIsAlreadyPBRPath
                        ? entry.slots
                        : toPluginSlots(slotsFromPBRRule(*matchedRule, entryVanillaDiffuse));
                }
            }
        }

        const auto ltexBaseSlots = toTextureSet(entry.slots);

        // Hoisted out of the season loop below - entry.txstEditorID doesn't change per season, so
        // this was being looked up up to 4x (once per season) for an identical result each time.
        const auto baseTextureSetConfigPath = findPBRTextureSetFile(pbrTextureSetIndex, entry.txstEditorID);

        for (const auto season : SEASONS) {
            // A record's own EDID/GNAM/grass-swap is one thing, but its TXST/texture is another -
            // a foreign mod covering this base for this season doesn't necessarily mean it did
            // anything to the TEXTURE (e.g. a grass mod's own duplicate may keep the exact same
            // base TXST, only swapping which Grasses list it carries). When that's the case, we
            // still generate our own seasonal texture duplicate as usual, but reuse THAT mod's own
            // Grasses list verbatim instead of guessing our own - so we don't fight its own,
            // deliberately-curated seasonal flora. borrowedGrassSource holds that mod's own
            // current LandscapeTextureEntry when this applies; empty otherwise (today's plain skip).
            optional<PGMutagenWrapper::LandscapeTextureEntry> borrowedGrassSource;
            if (foreignCoverage.isCovered(season, "LTEX", entry.modName, entry.formID, entry.editorID)) {
                if (const auto targetRaw
                    = foreignCoverage.getForeignTarget(season, "LTEX", entry.modName, entry.formID, entry.editorID);
                    targetRaw.has_value()) {
                    if (const auto found = resolveForeignLTEXTarget(*targetRaw, ltexByFormKey, ltexByEditorID);
                        found.has_value()
                        && boost::algorithm::to_lower_copy(found->slots.at(DIFFUSE_SLOT))
                            == boost::algorithm::to_lower_copy(entry.slots.at(DIFFUSE_SLOT))) {
                        borrowedGrassSource = found;
                    }
                }

                if (!borrowedGrassSource.has_value()) {
                    foreignCoverageSkipCount++;
                    continue; // genuinely another mod's own territory - leave it alone, as before
                }
            }

            const auto seasonResult = buildSeasonalSlots(pgd, ltexBaseSlots, season, terrainHelperPresent, pbrConfigIndex);
            if (!seasonResult.has_value()) {
                continue;
            }

            recordPBRRuleIfNeeded(ltexBaseSlots, *seasonResult);

            // Community Shaders PBRTextureSets config: only relevant when the base TextureSet has
            // one of its own to clone. The season TXST's EditorID must match exactly what
            // ASMutagen.cs's GetOrCreateSeasonalTXST() will generate for it - the base TXST's own
            // EditorID (e.g. "LandscapeDirt02") plus the season suffix, matching the vanilla/ESP
            // naming convention - so Community Shaders' by-EditorID lookup lines up. Falls back to
            // <season diffuse filename>_ltex only when the base TXST has no EditorID to build from.
            if (seasonResult->isPBR && entry.txstEditorID.empty()) {
                Logger::info(L"No TXST EditorID resolved for LTEX {} ({}) - can't look up a PBRTextureSets config",
                    entry.editorID, entry.modName);
            } else if (seasonResult->isPBR && !entry.txstEditorID.empty()) {
                if (baseTextureSetConfigPath.has_value()) {
                    const auto seasonTXSTEditorID = entry.txstEditorID + L"_" + wstring(season.begin(), season.end());

                    if (!seenPBRTextureSetFor.contains(seasonTXSTEditorID)
                        && !findPBRTextureSetFile(pbrTextureSetIndex, seasonTXSTEditorID).has_value()) {
                        seenPBRTextureSetFor[seasonTXSTEditorID] = true;

                        try {
                            outPBRTextureSets.push_back(PBRTextureSetFile {
                                .relativePath = filesystem::path(L"PBRTextureSets") / (seasonTXSTEditorID + L".json"),
                                .content = nlohmann::json::parse(bytesToString(pgd->getFile(*baseTextureSetConfigPath))),
                            });
                        } catch (const exception& e) {
                            Logger::warn(L"Failed to clone PBRTextureSets config for {}: {}", seasonTXSTEditorID,
                                StringUtil::utf8toUTF16(e.what()));
                        }
                    }
                } else {
                    Logger::info(
                        L"No PBRTextureSets config found for TXST EditorID \"{}\" (LTEX {})", entry.txstEditorID, entry.editorID);
                }
            }

            if (anyParallaxFound.empty() && !seasonResult->slots.at(PARALLAX_SLOT).empty()) {
                anyParallaxFound = seasonResult->slots.at(PARALLAX_SLOT);
            }

            PGMutagenWrapper::SeasonalTextureOverride override;
            override.slots = toPluginSlots(seasonResult->slots);

            PGMutagenWrapper::SeasonalDuplicateRequest req;
            req.modName = entry.modName;
            req.formID = entry.formID;
            req.type = "LTEX";
            req.seasonSuffix = string(season);
            req.baseTXSTEditorID = entry.txstEditorID;
            req.overrides.push_back(std::move(override));

            // Grass rides along with this same LTEX swap - Seasons of Skyrim has no ini
            // mechanism of its own for grass, since grass isn't placed via any REFR; it's just
            // whatever the currently-winning LTEX's own Grasses list says. So swapping which
            // grass a season's LTEX duplicate references is the entire mechanism.
            if (borrowedGrassSource.has_value()) {
                // Reuse the foreign mod's own curated seasonal grass list verbatim (even if
                // empty - that's a deliberate "no grass this season" choice too) instead of our
                // own per-slot mesh-sibling detection below: it's already exactly what that mod's
                // author intended, and second-guessing it would be a regression, not an improvement.
                req.overrideGrasses = true;
                req.grassOverrides = borrowedGrassSource->grasses;
                foreignRetargetCount++;
            } else if (!entry.grasses.empty()) {
                // Decided per grass slot, not per LTEX: a slot with its own seasonal mesh uses
                // it; a slot without one either falls back to no-grass-in-winter (if enabled and
                // this is winter) or just keeps its original, unswapped appearance. This way a
                // LTEX with e.g. 5 grass types where only 1 has a winter variant still drops the
                // other 4 in winter when the option is on, instead of leaving them green.
                bool changedAnyGrassSlot = false;
                vector<PGMutagenWrapper::FormKeyRef> newGrasses;
                newGrasses.reserve(entry.grasses.size());
                for (const auto& baseGrass : entry.grasses) {
                    if (const auto seasonalGrass = getOrCreateSeasonalGrass(baseGrass, season); seasonalGrass.has_value()) {
                        newGrasses.push_back(*seasonalGrass);
                        changedAnyGrassSlot = true;
                    } else if (removeGrassInWinter && season == "WIN") {
                        changedAnyGrassSlot = true; // dropped - no grass under snow for this slot
                    } else {
                        newGrasses.push_back(baseGrass); // no seasonal variant - keep the original grass
                    }
                }

                if (changedAnyGrassSlot) {
                    req.overrideGrasses = true;
                    req.grassOverrides = std::move(newGrasses);
                }
            }

            requestsByGroup[groupKey(season, "LTEX")].push_back(std::move(req));
        }
    }

    // Independent, not mutually exclusive: TerrainHelper's vanilla-parallax unlock and Community
    // Shaders' PBR terrain fallback are two unrelated conventions (PBR terrain doesn't need
    // TerrainHelper at all, even though TerrainHelper itself ships bundled with Community
    // Shaders) - a modlist could in principle need either, both, or neither.
    patchLandscapeDefaultParallax(pgd, anyParallaxFound);
    if (defaultPBRLandSlots.has_value()) {
        ensureDefaultPBRLand(*defaultPBRLandSlots);
    }

    if (seasonLockedSkipCount > 0) {
        Logger::info("Skipped {} record(s) with a season-locked EditorID (coast/river)", seasonLockedSkipCount);
    }
    if (foreignCoverageSkipCount > 0) {
        Logger::info(
            "Skipped {} record/season combination(s) already covered by another mod's Data/Seasons ini",
            foreignCoverageSkipCount);
    }
    if (foreignRetargetCount > 0) {
        Logger::info("Generated {} seasonal LTEX texture(s) for a record another mod already covers for grass, "
                     "reusing that mod's own seasonal grass list",
            foreignRetargetCount);
    }
    if (seasonalGrassSwapCount > 0) {
        Logger::info("Generated {} seasonal grass duplicate(s) from detected seasonal grass meshes", seasonalGrassSwapCount);
    }
    if (seasonalGrassReuseCount > 0) {
        Logger::info(
            "Reused {} mod-provided seasonal grass record(s) already present in the load order (e.g. \"Base_WIN\")",
            seasonalGrassReuseCount);
    }

    for (auto& [group, requests] : requestsByGroup) {
        if (requests.empty()) {
            continue;
        }

        const auto sepPos = group.find('|');
        const auto season = group.substr(0, sepPos);
        const auto recordType = group.substr(sepPos + 1);

        auto results = PGMutagenWrapper::libGenerateSeasonalDuplicates(requests);
        for (const auto& result : results) {
            if (result.newFormID == 0) {
                continue;
            }

            allSwaps.push_back(SwapEntry {
                .baseModName = result.modName,
                .baseFormID = result.formID,
                .newModName = result.newModName,
                .newFormID = result.newFormID,
                .season = season,
                .recordType = recordType,
            });
        }
    }

    outForeignRawLinesBySection = std::move(foreignCoverage.rawLinesBySection);
    outForeignIniFilenames = std::move(foreignCoverage.foreignIniFilenames);

    return allSwaps;
}

void SeasonPatcher::writeIniFiles(const std::filesystem::path& outputDir, const std::vector<SwapEntry>& swaps,
    const std::unordered_map<std::string, std::vector<std::wstring>>& foreignRawLinesBySection)
{
    if (swaps.empty() && foreignRawLinesBySection.empty()) {
        return;
    }

    // group by season
    unordered_map<string, vector<const SwapEntry*>> bySeason;
    for (const auto& swap : swaps) {
        bySeason[swap.season].push_back(&swap);
    }

    // Every season either side has content for - a foreign-only season (e.g. every tree in the
    // load order is already covered by another mod, so AutoSeasons generated zero duplicates of
    // its own for it) still needs its own AIO ini written.
    unordered_set<string> allSeasons;
    for (const auto& [season, entries] : bySeason) {
        allSeasons.insert(season);
    }
    for (const auto& [key, lines] : foreignRawLinesBySection) {
        allSeasons.insert(key.substr(0, key.find('|')));
    }

    const auto seasonsDir = outputDir / "Seasons";
    filesystem::create_directories(seasonsDir);

    // Section names match Seasons of Skyrim's FormSwapMap::recordNames exactly.
    static const array<pair<string_view, wstring_view>, 7> SECTION_NAMES { {
        { "STAT", L"Statics" },
        { "LTEX", L"LandTextures" },
        { "ACTI", L"Activators" },
        { "FURN", L"Furniture" },
        { "MSTT", L"MovableStatics" },
        { "TREE", L"Trees" },
        { "FLOR", L"Flora" },
    } };

    for (const auto& season : allSeasons) {
        // "z_" prefix: Seasons of Skyrim evaluates Data\Seasons\*.ini configs alphabetically, last
        // one wins for a given record - this keeps AutoSeasons' entries winning over any other
        // mod's own seasonal config that happens to target the same record (SoS's own
        // auto-generated MainFormSwap_WIN already always loses to any user config regardless of
        // name, so this is purely defense against other third-party seasonal ini files). "AIO"
        // (all-in-one): this file also folds in every other detected mod's own season-swap
        // declarations - see the class-level doc comment on writeForeignIniOverrides().
        const auto iniPath = seasonsDir / ("z_AIO_AutoSeasons_" + season + ".ini");

        wofstream out(iniPath, ios::out | ios::trunc);
        if (!out.is_open()) {
            continue;
        }

        // Register with PGDirectory's generated-file tracking so AutoSeasons recognizes this file
        // as its own output on the next run (otherwise it's flagged as a foreign/leftover file).
        PGGlobals::getPGD()->addGeneratedFile(filesystem::path("Seasons") / iniPath.filename());

        // group this season's entries by record type, so each gets its own section
        unordered_map<string, vector<const SwapEntry*>> byType;
        const auto seasonIt = bySeason.find(season);
        if (seasonIt != bySeason.end()) {
            for (const auto* entry : seasonIt->second) {
                byType[entry->recordType].push_back(entry);
            }
        }

        for (const auto& [recordType, sectionName] : SECTION_NAMES) {
            const auto typeEntriesIt = byType.find(string(recordType));
            const auto foreignLinesIt = foreignRawLinesBySection.find(season + "|" + string(recordType));
            if (typeEntriesIt == byType.end() && foreignLinesIt == foreignRawLinesBySection.end()) {
                continue; // nothing for this record type this season, from either source
            }

            out << L"[" << sectionName << L"]\n";

            const auto writeOurs = [&]() {
                if (typeEntriesIt != byType.end()) {
                    for (const auto* entry : typeEntriesIt->second) {
                        out << L"0x" << hex << uppercase << entry->baseFormID << L"~" << entry->baseModName << L"|"
                            << L"0x" << entry->newFormID << L"~" << entry->newModName << L"\n"
                            << dec;
                    }
                }
            };
            const auto writeForeign = [&]() {
                if (foreignLinesIt != foreignRawLinesBySection.end()) {
                    for (const auto& line : foreignLinesIt->second) {
                        out << line << L"\n";
                    }
                }
            };

            // LTEX only: written in the opposite order (foreign lines first, ours last) because
            // SeasonPatcher::run() can now generate our own texture-only duplicate for a base LTEX
            // a foreign mod ALSO declares (its own grass-only patch, reusing the base texture
            // unchanged - see the borrowedGrassSource logic in run()'s LTEX loop). When that
            // happens, both a foreign line and our own line target the same base record in this
            // same file, and Seasons of Skyrim's ini parser keeps the LAST value for a given key -
            // ours needs to be that last value so our seasonal texture wins while still carrying
            // that mod's own curated Grasses list. Every other record type never has this overlap
            // (a covered record with no retarget is skipped entirely, so only one side ever writes
            // a line for a given base), so their relative order still doesn't matter.
            if (recordType == "LTEX") {
                writeForeign();
                writeOurs();
            } else {
                writeOurs();
                writeForeign();
            }
        }
    }
}

void SeasonPatcher::writeForeignIniOverrides(
    const std::filesystem::path& outputDir, const std::vector<std::filesystem::path>& foreignIniFilenames)
{
    if (foreignIniFilenames.empty()) {
        return;
    }

    const auto seasonsDir = outputDir / "Seasons";
    filesystem::create_directories(seasonsDir);

    for (const auto& filename : foreignIniFilenames) {
        const auto iniPath = seasonsDir / filename;

        wofstream out(iniPath, ios::out | ios::trunc);
        if (!out.is_open()) {
            continue;
        }

        out << L"; Superseded by z_AIO_AutoSeasons_<SEASON>.ini - AutoSeasons requires its own "
               L"output mod to load after this one, so this empty file overrides the original and "
               L"its swaps aren't evaluated twice.\n";

        PGGlobals::getPGD()->addGeneratedFile(filesystem::path("Seasons") / filename);
    }
}

void SeasonPatcher::writePBRJsonRules(
    const std::filesystem::path& outputDir, const std::vector<PBRJsonRuleFile>& rules)
{
    for (const auto& ruleFile : rules) {
        const auto fullPath = outputDir / ruleFile.relativePath;
        filesystem::create_directories(fullPath.parent_path());

        nlohmann::json output = nlohmann::json::array();
        output.push_back(ruleFile.rule);

        ofstream out(fullPath, ios::out | ios::trunc);
        if (!out.is_open()) {
            continue;
        }
        out << output.dump(2);

        PGGlobals::getPGD()->addGeneratedFile(ruleFile.relativePath);
    }
}

void SeasonPatcher::writePBRTextureSetFiles(
    const std::filesystem::path& outputDir, const std::vector<PBRTextureSetFile>& files)
{
    for (const auto& file : files) {
        const auto fullPath = outputDir / file.relativePath;
        filesystem::create_directories(fullPath.parent_path());

        ofstream out(fullPath, ios::out | ios::trunc);
        if (!out.is_open()) {
            continue;
        }
        out << file.content.dump(2); // a bare object, unlike PBRNIFPatcher's array-of-rules files

        PGGlobals::getPGD()->addGeneratedFile(file.relativePath);
    }
}
