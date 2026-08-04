#include "SeasonPatcher.hpp"

#include "PGGlobals.hpp"
#include "PGMutagenWrapper.hpp"
#include "pgutil/PGNIFUtil.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"

#include "NifFile.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
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
// defaulting to "coast"/"river" - a subset of Seasons of Skyrim's own automatic winter form-swap
// generator's EditorID blacklist, GenerateLandTextureSnowVariant, which also lists
// "snow"/"ice"/"winter"/"frozen"). Only coast/river are the default here: unlike snow/ice/winter/
// frozen objects (which some users deliberately still want seasonal duplicates for - e.g. a
// dedicated "_SUM" variant of a snow-named static), coast/river-named objects are never meant to
// get a seasonal duplicate at all, regardless of which season-suffixed texture files happen to
// exist on disk. Exposed as a GUI/config list (like the mesh blocklist) rather than hardcoded, so
// mod authors can tailor it to their own naming conventions.
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
    if (season == "WIN") {
        return { L"win", L"winter" };
    }
    if (season == "SPR") {
        return { L"spr", L"spring" };
    }
    if (season == "SUM") {
        return { L"sum", L"summer" };
    }
    if (season == "AUT") {
        return { L"aut", L"autumn", L"fall" };
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
        auto configFileBytes = pgd->getFile(configPath);
        string configFileStr;
        ranges::transform(
            configFileBytes, back_inserter(configFileStr), [](std::byte b) { return static_cast<char>(b); });

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
        } catch (const nlohmann::json::parse_error&) {
            continue; // malformed config - PGPatcher's own downstream pass will report it
        }
    }

    return entries;
}

// Finds an existing PBR rule whose match_diffuse matches the given base (non-seasonal) diffuse
// path, to clone its non-matching attributes (roughness_scale, subsurface, multilayer, etc.) for
// a new seasonal rule. A simple suffix-match linear scan - this list is small (at most a few
// hundred entries for a real modlist) and only walked for confirmed seasonal-PBR candidates.
auto findMatchingPBRConfig(const vector<nlohmann::json>& configs, const wstring& baseDiffusePath)
    -> optional<nlohmann::json>
{
    const auto lowerPath = boost::algorithm::to_lower_copy(baseDiffusePath);

    for (const auto& cfg : configs) {
        if (!cfg.contains("match_diffuse")) {
            continue;
        }

        auto matchDiffuse = StringUtil::utf8toUTF16(cfg["match_diffuse"].get<string>());
        boost::algorithm::to_lower(matchDiffuse);
        if (matchDiffuse.starts_with(L"\\")) {
            matchDiffuse = matchDiffuse.substr(1);
        }

        if (!matchDiffuse.empty() && lowerPath.size() >= matchDiffuse.size() && lowerPath.ends_with(matchDiffuse)) {
            return cfg;
        }
    }

    return nullopt;
}

// Maps a texture path to the PBRNIFPatcher json file it should live in, mirroring the texture's
// own subfolder: "textures\landscape\dirt02_win.dds" -> "PBRNIFPatcher\landscape\dirt02_win.json".
auto toPBRJsonRelativePath(const std::wstring& texturePath) -> std::filesystem::path
{
    const filesystem::path texPath(texturePath);
    auto folder = texPath.parent_path().wstring();
    boost::algorithm::to_lower(folder);

    const wstring texturesPrefix = L"textures\\";
    if (folder.starts_with(texturesPrefix)) {
        folder = folder.substr(texturesPrefix.size());
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
    for (int i = 1; i <= 9; i++) {
        rule.erase("match" + to_string(i));
    }
    rule.erase("meta_matchedFrom");

    rule["match_diffuse"] = StringUtil::utf16toUTF8(seasonDiffuseFileName);
    return rule;
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

// Reads every "*_<SUFFIX>.ini" file already present under Data/Seasons (Seasons of Skyrim's own
// filename convention - a case-insensitive WIN/SPR/SUM/AUT suffix is what tells it which season a
// config applies to), skipping our own output (the "z_AutoSeasons_" prefix - regenerated every
// run, not "someone else's" data), and returns the set of (season, recordType, modName, formID)
// keys already covered by another mod's ini. Used so run() can skip generating a redundant
// AutoSeasons duplicate for a record another mod already handles for that season - harmless
// either way since our "z_" prefix makes ours win if both exist, but avoids wasting a FormID and
// producing a visually-identical duplicate record for nothing.
auto loadForeignSeasonCoverage(const filesystem::path& seasonsDir) -> unordered_set<wstring>
{
    unordered_set<wstring> coverage;
    if (!filesystem::exists(seasonsDir) || !filesystem::is_directory(seasonsDir)) {
        return coverage;
    }

    // Mirrors writeIniFiles()'s own sectionNames map, in reverse.
    static const unordered_map<string, string> sectionToRecordType { { "statics", "STAT" },
        { "landtextures", "LTEX" }, { "activators", "ACTI" }, { "furniture", "FURN" },
        { "movablestatics", "MSTT" }, { "trees", "TREE" }, { "flora", "FLOR" } };

    for (const auto& dirEntry : filesystem::directory_iterator(seasonsDir)) {
        if (!dirEntry.is_regular_file() || boost::algorithm::to_lower_copy(dirEntry.path().extension().wstring()) != L".ini") {
            continue;
        }

        const auto stem = boost::algorithm::to_lower_copy(dirEntry.path().stem().wstring());
        if (stem.starts_with(L"z_autoseasons_")) {
            continue; // our own output
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

            // "0x<HexFormID>~Plugin.esp|0x<HexFormID>~Plugin.esp" - only the left (base) side
            // matters here, since that's the record another mod is already overriding.
            const auto pipePos = line.find('|');
            const auto leftSide = pipePos != string::npos ? line.substr(0, pipePos) : line;
            const auto tildePos = leftSide.find('~');
            if (tildePos == string::npos) {
                continue;
            }

            unsigned int formID = 0;
            try {
                formID = static_cast<unsigned int>(stoul(leftSide.substr(0, tildePos), nullptr, 16));
            } catch (const exception&) {
                continue;
            }

            const auto modName = StringUtil::utf8toUTF16(leftSide.substr(tildePos + 1));
            coverage.insert(composeForeignCoverageKey(season, currentRecordType, modName, formID));
        }
    }

    return coverage;
}
}

auto SeasonPatcher::buildSeasonalSlots(PGDirectory* pgd,
                                       const PGTypes::TextureSet& baseSlots,
                                       std::string_view season,
                                       bool includeParallax) -> std::optional<SeasonalSlotResult>
{
    if (baseSlots.at(DIFFUSE_SLOT).empty()) {
        // no diffuse texture at all, nothing to key the seasonal lookup off of
        return nullopt;
    }

    if (isUnderPBRFolder(filesystem::path(baseSlots.at(DIFFUSE_SLOT)))) {
        // Already PBR-patched in-place (e.g. by an earlier PGPatcher run baked into the same load
        // order) - not handled here, would need a different (not-yet-supported) approach.
        return nullopt;
    }

    // 1. Vanilla-folder check first (covers vanilla, parallax, and complex-material textures -
    // all of which share the same diffuse+normal slot layout; PGPatcher's own downstream pass
    // adds parallax/CM slots on top of this if the right _p/_m files exist).
    const auto diffuseSeasonal = seasonSuffixedSibling(pgd, baseSlots.at(DIFFUSE_SLOT), PGEnums::TextureSlots::DIFFUSE, season);
    if (!diffuseSeasonal.empty()) {
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

    // 2. No vanilla-folder seasonal sibling - check for a PBR-folder seasonal variant of the same
    // base texture (e.g. textures\pbr\landscape\dirt02_aut.dds).
    const auto pbrDiffusePath = toPBRFolderPath(filesystem::path(baseSlots.at(DIFFUSE_SLOT))).wstring();
    const auto pbrDiffuseSeasonal = seasonSuffixedSibling(pgd, pbrDiffusePath, PGEnums::TextureSlots::DIFFUSE, season);
    if (pbrDiffuseSeasonal.empty()) {
        return nullopt; // no seasonal variant at all, vanilla or PBR
    }

    // Author a vanilla-style PLACEHOLDER diffuse/normal path (it doesn't need to exist on disk) -
    // PGPatcher's own downstream PBR pass rewrites these into the real textures\pbr\... paths
    // once it sees the matching json rule this class authors for this season-suffixed name.
    const auto vanillaStyleDiffuse = toVanillaFolderPath(filesystem::path(pbrDiffuseSeasonal));
    auto vanillaDiffuseStr = vanillaStyleDiffuse.wstring();

    SeasonalSlotResult result;
    result.isPBR = true;
    result.slots.at(DIFFUSE_SLOT) = vanillaDiffuseStr;
    result.seasonDiffusePath = vanillaDiffuseStr;

    if (vanillaDiffuseStr.size() > vanillaStyleDiffuse.extension().wstring().size()) {
        const auto stem = vanillaDiffuseStr.substr(0, vanillaDiffuseStr.size() - vanillaStyleDiffuse.extension().wstring().size());
        result.slots.at(NORMAL_SLOT) = stem + L"_n.dds";
    }

    return result;
}

auto SeasonPatcher::run(PGDirectory* pgd, const std::vector<std::wstring>& meshBlockList,
    const std::vector<std::wstring>& seasonLockedEditorIDKeywords, bool removeGrassInWinter,
    std::vector<PBRJsonRuleFile>& outPBRRules) -> std::vector<SwapEntry>
{
    vector<SwapEntry> allSwaps;
    size_t seasonLockedSkipCount = 0;
    size_t foreignCoverageSkipCount = 0;
    const auto foreignCoverage = loadForeignSeasonCoverage(pgd->getDataPath() / "Seasons");

    // Batch requests per (season, recordType) so we make one Mutagen call per group instead of
    // one per record, and so results (which don't echo record type) can still be attributed
    // correctly when building SwapEntry.
    unordered_map<string, vector<PGMutagenWrapper::SeasonalDuplicateRequest>> requestsByGroup;
    const auto groupKey = [](string_view season, string_view recordType) -> string {
        return string(season) + "|" + string(recordType);
    };

    // Lazily loaded (only needed if a PBR-folder seasonal variant is actually found), and cached
    // for the whole pass since it's cheap and the same list applies to every season/mesh.
    optional<vector<nlohmann::json>> pbrConfigs;
    unordered_map<wstring, bool> seenPBRRuleFor; // dedup: one rule (and file) per unique seasonal diffuse texture

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

        if (!pbrConfigs.has_value()) {
            pbrConfigs = loadAllPBRConfigEntries(pgd);
        }

        const auto matchedBase = findMatchingPBRConfig(*pbrConfigs, baseSlots.at(DIFFUSE_SLOT));
        const auto seasonDiffuseFileName = filesystem::path(seasonResult.seasonDiffusePath).filename().wstring();

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

    const auto getOrCreateSeasonalGrass
        = [&](const PGMutagenWrapper::FormKeyRef& baseGrass, string_view season) -> optional<PGMutagenWrapper::FormKeyRef> {
        const auto cacheKey = boost::algorithm::to_lower_copy(baseGrass.modName) + L"|" + to_wstring(baseGrass.formID)
            + L"|" + wstring(season.begin(), season.end());
        if (const auto it = grassDuplicateCache.find(cacheKey); it != grassDuplicateCache.end()) {
            return it->second;
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

    for (const auto& [meshPath, cache] : pgd->getMeshes()) {
        if (!meshBlockList.empty() && PGDirectory::checkGlobMatchInVector(meshPath.wstring(), meshBlockList)) {
            continue; // e.g. interior meshes, which Seasons of Skyrim never swaps
        }

        optional<vector<ShapeTextureInfo>> defaultShapes; // lazily loaded from the NIF, at most once per mesh

        for (const auto& [formKey, attrs] : cache.meshUses) {
            // Seasons of Skyrim's FormSwapMap supports these six model-referencing record types
            // (plus VisualEffects/RFCT, not handled here - RFCT records don't carry a Model of
            // their own, they point at a separate ArtObject, so they don't fit this same
            // Model+AlternateTextures duplication approach).
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

            // One base entry per shape that needs its own AlternateTextures override: either the
            // record's existing overrides (keyed by their real plugin Index, name left empty since
            // we're matching an existing entry, not authoring a new one), or - if the record has no
            // overrides at all - every shape read directly from the NIF (index + name needed since
            // we may have to author brand-new AlternateTextures entries for them).
            vector<ShapeTextureInfo> baseShapes;
            if (!attrs.alternateTextures.empty()) {
                baseShapes.reserve(attrs.alternateTextures.size());
                for (const auto& [shapeIndex, textureSet] : attrs.alternateTextures) {
                    baseShapes.push_back(ShapeTextureInfo {
                        .shapeIndex = static_cast<int>(shapeIndex),
                        .shapeName = L"",
                        .slots = textureSet,
                    });
                }
            } else {
                if (!defaultShapes.has_value()) {
                    defaultShapes = getAllShapeSlotsFromMesh(pgd, meshPath);
                }

                baseShapes = *defaultShapes;
            }

            if (baseShapes.empty()) {
                continue;
            }

            for (const auto season : SEASONS) {
                if (foreignCoverage.contains(composeForeignCoverageKey(season, recTypeStr, formKey.modKey, formKey.formID))) {
                    foreignCoverageSkipCount++;
                    continue; // another mod's Data/Seasons ini already covers this record for this season
                }

                vector<PGMutagenWrapper::SeasonalTextureOverride> overrides;

                for (const auto& baseShape : baseShapes) {
                    const auto seasonResult = buildSeasonalSlots(pgd, baseShape.slots, season, false);
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
    const bool terrainHelperPresent = !PGMutagenWrapper::libGetLandscapeDefaultDiffuse().empty();

    wstring anyParallaxFound; // first seasonal parallax texture encountered, for LandscapeDefault
    for (const auto& entry : PGMutagenWrapper::libEnumerateLandscapeTextures()) {
        if (entry.slots.at(DIFFUSE_SLOT).empty()) {
            continue;
        }

        if (hasSeasonLockedEditorID(entry.editorID, seasonLockedEditorIDKeywords)) {
            seasonLockedSkipCount++;
            continue;
        }

        const auto ltexBaseSlots = toTextureSet(entry.slots);

        for (const auto season : SEASONS) {
            if (foreignCoverage.contains(composeForeignCoverageKey(season, "LTEX", entry.modName, entry.formID))) {
                foreignCoverageSkipCount++;
                continue; // another mod's Data/Seasons ini already covers this record for this season
            }

            const auto seasonResult = buildSeasonalSlots(pgd, ltexBaseSlots, season, terrainHelperPresent);
            if (!seasonResult.has_value()) {
                continue;
            }

            recordPBRRuleIfNeeded(ltexBaseSlots, *seasonResult);

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
            req.overrides.push_back(std::move(override));

            // Grass rides along with this same LTEX swap - Seasons of Skyrim has no ini
            // mechanism of its own for grass, since grass isn't placed via any REFR; it's just
            // whatever the currently-winning LTEX's own Grasses list says. So swapping which
            // grass a season's LTEX duplicate references is the entire mechanism.
            if (!entry.grasses.empty()) {
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

    patchLandscapeDefaultParallax(pgd, anyParallaxFound);

    if (seasonLockedSkipCount > 0) {
        Logger::info("Skipped {} record(s) with a season-locked EditorID (coast/river)", seasonLockedSkipCount);
    }
    if (foreignCoverageSkipCount > 0) {
        Logger::info(
            "Skipped {} record/season combination(s) already covered by another mod's Data/Seasons ini",
            foreignCoverageSkipCount);
    }
    if (seasonalGrassSwapCount > 0) {
        Logger::info("Generated {} seasonal grass duplicate(s) from detected seasonal grass meshes", seasonalGrassSwapCount);
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

    return allSwaps;
}

void SeasonPatcher::writeIniFiles(const std::filesystem::path& outputDir, const std::vector<SwapEntry>& swaps)
{
    if (swaps.empty()) {
        return;
    }

    // group by season
    unordered_map<string, vector<const SwapEntry*>> bySeason;
    for (const auto& swap : swaps) {
        bySeason[swap.season].push_back(&swap);
    }

    const auto seasonsDir = outputDir / "Seasons";
    filesystem::create_directories(seasonsDir);

    // Section names match Seasons of Skyrim's FormSwapMap::recordNames exactly.
    const unordered_map<string, wstring> sectionNames {
        { "STAT", L"Statics" },
        { "LTEX", L"LandTextures" },
        { "ACTI", L"Activators" },
        { "FURN", L"Furniture" },
        { "MSTT", L"MovableStatics" },
        { "TREE", L"Trees" },
        { "FLOR", L"Flora" },
    };

    for (const auto& [season, entries] : bySeason) {
        // "z_" prefix: Seasons of Skyrim evaluates Data\Seasons\*.ini configs alphabetically, last
        // one wins for a given record - this keeps AutoSeasons' entries winning over any other
        // mod's own seasonal config that happens to target the same record (SoS's own
        // auto-generated MainFormSwap_WIN already always loses to any user config regardless of
        // name, so this is purely defense against other third-party seasonal ini files).
        const auto iniPath = seasonsDir / ("z_AutoSeasons_" + season + ".ini");

        wofstream out(iniPath, ios::out | ios::trunc);
        if (!out.is_open()) {
            continue;
        }

        // Register with PGDirectory's generated-file tracking so AutoSeasons recognizes this file
        // as its own output on the next run (otherwise it's flagged as a foreign/leftover file).
        PGGlobals::getPGD()->addGeneratedFile(filesystem::path("Seasons") / iniPath.filename());

        // group this season's entries by record type, so each gets its own section
        unordered_map<string, vector<const SwapEntry*>> byType;
        for (const auto* entry : entries) {
            byType[entry->recordType].push_back(entry);
        }

        for (const auto& [recordType, typeEntries] : byType) {
            const auto sectionIt = sectionNames.find(recordType);
            if (sectionIt == sectionNames.end()) {
                continue;
            }

            out << L"[" << sectionIt->second << L"]\n";
            for (const auto* entry : typeEntries) {
                out << L"0x" << hex << uppercase << entry->baseFormID << L"~" << entry->baseModName << L"|"
                    << L"0x" << entry->newFormID << L"~" << entry->newModName << L"\n"
                    << dec;
            }
        }
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
