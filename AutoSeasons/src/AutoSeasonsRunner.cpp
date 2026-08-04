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

    spdlog::info("Initializing plugin patching");
    PGPlugin::initialize(bg, exePath);
    PGPlugin::populateObjs();

    spdlog::info("Reading NIFs and textures");
    pgd.populateFileMap(false);
    pgd.mapFiles({}, {}, {}, {}, multithreading);

    // mapFiles() queues per-mesh model-use lookups (which STAT/etc. records reference each mesh)
    // onto a background task queue rather than resolving them inline. PGPatcher's own pipeline
    // has enough other work between mapFiles() and reading that data that the queue happens to
    // have drained by the time it matters; we go straight to SeasonPatcher::run(), so without an
    // explicit wait here that data is still incomplete and STAT detection silently finds nothing.
    spdlog::info("Waiting for mesh-use mapping to finish");
    pgd.waitForMeshMapping();
    pgd.waitForCMClassification();

    spdlog::info("Generating seasonal variation");
    std::vector<SeasonPatcher::PBRJsonRuleFile> pbrRules;
    const auto seasonSwaps = SeasonPatcher::run(
        &pgd, params.meshBlockList, params.seasonLockedEditorIDKeywords, params.removeGrassInWinter, pbrRules);
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

    SeasonPatcher::writeIniFiles(params.outputDir, seasonSwaps);
    if (!pbrRules.empty()) {
        spdlog::info("Seasonal variation: authored {} PBR json rule file(s) for a later PGPatcher run", pbrRules.size());
        SeasonPatcher::writePBRJsonRules(params.outputDir, pbrRules);
    }

    spdlog::info("Saving plugin");
    PGPlugin::savePlugin(params.outputDir, static_cast<PGPlugin::ESMMode>(params.esmMode));

    spdlog::info("Done!");
}
