#pragma once

#include "ASConfig.hpp"

#include <spdlog/common.h>

#include <filesystem>
#include <vector>

/**
 * @brief The actual patch pipeline (GPU init, plugin init, file scan, seasonal variation, save),
 * shared between the CLI and GUI entry points so they can't drift apart.
 */
namespace AutoSeasonsRunner {

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
 * @throws std::exception on any failure; callers are expected to catch and report it themselves.
 */
void run(ASParams& params,
        const std::filesystem::path& exePath,
        const std::vector<spdlog::sink_ptr>& extraSinks,
        bool multithreading = true);

}
