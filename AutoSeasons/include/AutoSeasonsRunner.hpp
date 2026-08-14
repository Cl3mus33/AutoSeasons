#pragma once

#include "ASConfig.hpp"

#include <spdlog/common.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief The actual patch pipeline (GPU init, plugin init, file scan, seasonal variation, save),
 * shared between the CLI and GUI entry points so they can't drift apart.
 */
namespace AutoSeasonsRunner {

/**
 * @struct RunSummary
 * @brief What a run actually produced - lets a caller show a real "0 records" vs "1,842 records"
 * completion message instead of a generic "success" that looks identical either way.
 */
struct RunSummary {
    size_t totalRecords = 0;
    /// Per-record-type counts, in a fixed display order (STAT, LTEX, ACTI, FURN, MSTT, TREE, FLOR).
    std::vector<std::pair<std::string, size_t>> countsByType;
    /// Record/season combinations covered by 2+ foreign mods with no priority order between them
    /// (see SeasonPatcher::run()'s outUnresolvedForeignConflictCount) - 0 means no such conflicts.
    size_t unresolvedForeignConflicts = 0;
};

/**
 * @brief Runs the full AutoSeasons pipeline.
 *
 * Resolves params.gameDir/params.outputDir to absolute paths (mutating params), sets up logging
 * (always includes a file sink at <output>/AutoSeasons.log, plus whatever extraSinks the caller
 * wants - e.g. a console sink for the CLI, or a GUI log-panel sink), then runs GPU init, plugin
 * init, mesh/texture scanning, the seasonal-variation pass, and saves the output plugin.
 *
 * @param params Config to run with; gameDir/outputDir are rewritten to their absolute form.
 * @param exePath Directory the executable lives in (used to locate cshaders/ and dotnetlib/).
 * @param extraSinks Additional spdlog sinks to log to, beyond the always-on output-dir file sink.
 * @param multithreading Whether to multithread the mesh/texture scan.
 * @return A summary of what was generated (or would have been, for a dry run - see
 * params.dryRun), for callers that want to show it (e.g. the GUI's completion message) - safe to
 * ignore if all a caller needs is the log file.
 * @throws std::exception on any failure; callers are expected to catch and report it themselves.
 */
auto run(ASParams& params,
        const std::filesystem::path& exePath,
        const std::vector<spdlog::sink_ptr>& extraSinks,
        bool multithreading = true) -> RunSummary;

}
