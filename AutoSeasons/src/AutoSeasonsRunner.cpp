#include "AutoSeasonsRunner.hpp"

#include "PGD3D.hpp"
#include "PGDirectory.hpp"
#include "PGGlobals.hpp"
#include "PGPlugin.hpp"
#include "SeasonPatcher.hpp"
#include "common/BethesdaGame.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

using namespace std;

void AutoSeasonsRunner::run(ASParams& params,
                            const filesystem::path& exePath,
                            const vector<spdlog::sink_ptr>& extraSinks,
                            bool multithreading)
{
    // Resolve to absolute paths immediately - relative paths depend on whatever working
    // directory the process happened to start in (which varies a lot when launched via MO2 or a
    // shortcut), so without this it's easy to lose track of where output actually went.
    params.gameDir = filesystem::absolute(params.gameDir);
    params.outputDir = filesystem::absolute(params.outputDir);

    filesystem::create_directories(params.outputDir);

    // Log to a file inside the output directory (always) plus whatever the caller wants on top
    // (console for the CLI, a GUI log panel, etc). Console windows launched via MO2/Explorer
    // close the instant the process exits, so without a durable file there'd be no way to check
    // afterward what happened or where files went.
    const auto logPath = params.outputDir / "AutoSeasons.log";
    vector<spdlog::sink_ptr> sinks = extraSinks;
    sinks.push_back(make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.wstring(), true));
    const auto logger = make_shared<spdlog::logger>("AutoSeasons", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::info("Welcome to AutoSeasons!");
    spdlog::info("Game directory: {}", params.gameDir.string());
    spdlog::info("Output directory: {}", params.outputDir.string());
    spdlog::info("Log file: {}", logPath.string());

    auto bg = BethesdaGame(params.gameType, params.gameDir);

    auto pgd = PGDirectory(&bg, params.outputDir);
    PGGlobals::setPGD(&pgd);

    auto pgd3D = PGD3D(exePath / "AutoSeasons_cshaders");
    PGGlobals::setPGD3D(&pgd3D);

    if (!pgd3D.initGPU()) {
        throw runtime_error("Failed to initialize GPU.");
    }

    if (!pgd3D.initShaders()) {
        throw runtime_error("Failed to initialize internal shaders.");
    }

    if (filesystem::equivalent(params.outputDir, bg.getGameDataPath())) {
        throw runtime_error("Output directory cannot be the same directory as your game's data folder.");
    }

    // Refuse to run while a previous run's own output is still active in the merged Data view
    // (e.g. the "AutoSeasons Output" mod is still enabled in MO2). Scanning its own previous
    // output as if it were a real mod can produce wrong results (its PBRNIFPatcher/PBRTextureSets
    // files look like "another mod already has a rule for this") and, per user reports, can make
    // the scan dramatically slower. Checks every file that currently exists in outputDir (not just
    // AutoSeasons.esp by name) against its same-relative-path counterpart in the merged Data view,
    // stopping at the first one that's literally the same physical file - not just same-named -
    // since a user could have manually deleted just the esp while leaving other generated files
    // (Seasons ini, PBRNIFPatcher/PBRTextureSets json) in place, and any one of those still being
    // live in the merge is just as much of a problem. A leftover, unrelated AutoSeasons.esp a user
    // copied into their real Data folder some other way is a different physical file and never
    // matches, so this doesn't false-positive on that.
    if (filesystem::exists(params.outputDir)) {
        for (const auto& entry : filesystem::recursive_directory_iterator(params.outputDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const auto relativePath = filesystem::relative(entry.path(), params.outputDir);
            const auto mergedPath = bg.getGameDataPath() / relativePath;

            error_code equivEc;
            if (filesystem::exists(mergedPath) && filesystem::equivalent(mergedPath, entry.path(), equivEc) && !equivEc) {
                throw runtime_error(
                    "AutoSeasons' own output mod is still enabled in your mod manager (detected via \""
                    + relativePath.string()
                    + "\"). Please disable it (e.g. the \"AutoSeasons Output\" mod in MO2) before starting a "
                      "new generation, then re-enable it once this run has finished - otherwise this run "
                      "would scan its own previous output as if it were a real mod, which can produce "
                      "incorrect results and slow the scan down a lot.");
            }
        }
    }

    // Clear this tool's own output from any previous run before regenerating - otherwise a file a
    // prior run created (e.g. a PBRNIFPatcher rule for a texture no longer detected after a
    // blocklist or load order change) lingers forever, since nothing else ever removes it.
    // Deliberately scoped to exactly the artifact names/folders AutoSeasons itself ever writes,
    // never a blanket wipe of outputDir - the equivalent() check above already guarantees this
    // isn't the game's real Data folder, but outputDir could still in principle be a folder a user
    // repurposed for something else too.
    spdlog::info("Clearing previous output");
    for (const auto& relPath : { filesystem::path("Seasons"), filesystem::path("PBRNIFPatcher"), filesystem::path("PBRTextureSets") }) {
        error_code deleteEc;
        filesystem::remove_all(params.outputDir / relPath, deleteEc);
        if (deleteEc) {
            spdlog::warn("Could not fully clear previous \"{}\" output ({}) - it may be open in another "
                         "program (e.g. xEdit)",
                relPath.string(), deleteEc.message());
        }
    }
    {
        error_code deleteEc;
        filesystem::remove(params.outputDir / "AutoSeasons.esp", deleteEc);
        if (deleteEc) {
            spdlog::warn("Could not remove previous AutoSeasons.esp ({}) - it may be open in another "
                         "program (e.g. xEdit)",
                deleteEc.message());
        }
    }

    spdlog::info("Initializing plugin patching");
    PGPlugin::initialize(bg, exePath);
    PGPlugin::populateObjs();

    spdlog::info("Reading NIFs and textures");
    pgd.populateFileMap(false);
    pgd.mapFiles({}, {}, {}, {}, multithreading);

    pgd.waitForCMClassification();

    spdlog::info("Generating seasonal variation");
    std::vector<SeasonPatcher::PBRJsonRuleFile> pbrRules;
    std::vector<SeasonPatcher::PBRTextureSetFile> pbrTextureSets;
    std::unordered_map<std::string, std::vector<std::wstring>> foreignRawLinesBySection;
    std::vector<std::filesystem::path> foreignIniFilenames;
    const auto seasonSwaps = SeasonPatcher::run(&pgd, params.meshBlockList, params.seasonLockedEditorIDKeywords,
        params.removeGrassInWinter, pbrRules, pbrTextureSets, foreignRawLinesBySection, foreignIniFilenames);
    spdlog::info("Seasonal variation: created {} duplicate record(s)", seasonSwaps.size());

    // Per-record-type breakdown - lets the user tell "this type genuinely has no seasonal
    // content in this load order" apart from "something's wrong with detection for this type".
    std::map<std::string, size_t> countsByType;
    for (const auto& swap : seasonSwaps) {
        countsByType[swap.recordType]++;
    }
    static constexpr std::array<const char*, 7> RECORD_TYPE_ORDER { "STAT", "LTEX", "ACTI", "FURN", "MSTT", "TREE",
        "FLOR" };
    std::string breakdown;
    for (const auto* recordType : RECORD_TYPE_ORDER) {
        if (!breakdown.empty()) {
            breakdown += ", ";
        }
        breakdown += string(recordType) + ": " + to_string(countsByType[recordType]);
    }
    spdlog::info("Seasonal variation breakdown: {}", breakdown);

    SeasonPatcher::writeIniFiles(params.outputDir, seasonSwaps, foreignRawLinesBySection);
    if (!foreignIniFilenames.empty()) {
        spdlog::info("Seasonal variation: folded {} other mod(s') own Data/Seasons ini into the merged AIO "
                     "output and wrote a neutralizing override for each",
            foreignIniFilenames.size());
        SeasonPatcher::writeForeignIniOverrides(params.outputDir, foreignIniFilenames);
    }
    if (!pbrRules.empty()) {
        spdlog::info("Seasonal variation: authored {} PBR json rule file(s) for a later PGPatcher run", pbrRules.size());
        SeasonPatcher::writePBRJsonRules(params.outputDir, pbrRules);
    }
    if (!pbrTextureSets.empty()) {
        spdlog::info("Seasonal variation: authored {} PBRTextureSets config(s) for Community Shaders", pbrTextureSets.size());
        SeasonPatcher::writePBRTextureSetFiles(params.outputDir, pbrTextureSets);
    }

    spdlog::info("Saving plugin");
    PGPlugin::savePlugin(params.outputDir, static_cast<PGPlugin::ESMMode>(params.esmMode));

    spdlog::info("Done!");
}
