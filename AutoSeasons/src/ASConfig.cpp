#include "ASConfig.hpp"

#include "util/FileUtil.hpp"
#include "util/Logger.hpp"
#include "util/StringUtil.hpp"

#include <nlohmann/json.hpp>

#include <fstream>

using namespace std;

namespace {
auto gameTypeToString(const BethesdaGame::GameType& type) -> string
{
    switch (type) {
    case BethesdaGame::GameType::SKYRIM_GOG:
        return "skyrimgog";
    case BethesdaGame::GameType::SKYRIM_VR:
        return "skyrimvr";
    case BethesdaGame::GameType::ENDERAL_SE:
        return "enderalse";
    case BethesdaGame::GameType::SKYRIM_SE:
    case BethesdaGame::GameType::UNKNOWN:
    default:
        return "skyrimse";
    }
}

auto gameTypeFromString(const string& str) -> BethesdaGame::GameType
{
    if (str == "skyrimgog") {
        return BethesdaGame::GameType::SKYRIM_GOG;
    }
    if (str == "skyrimvr") {
        return BethesdaGame::GameType::SKYRIM_VR;
    }
    if (str == "enderalse") {
        return BethesdaGame::GameType::ENDERAL_SE;
    }
    return BethesdaGame::GameType::SKYRIM_SE;
}
}

auto ASConfig::getConfigPath(const filesystem::path& exeDir) -> filesystem::path
{
    return exeDir / "AutoSeasons_config.json";
}

auto ASConfig::load(const filesystem::path& exeDir) -> ASParams
{
    ASParams params;

    nlohmann::json configJ;
    if (!FileUtil::getJSON(getConfigPath(exeDir), configJ)) {
        return params;
    }

    try {
        if (configJ.contains("uiLanguage")) {
            params.uiLanguage = configJ["uiLanguage"].get<string>();
        }
        if (configJ.contains("gameDir")) {
            params.gameDir = StringUtil::utf8toUTF16(configJ["gameDir"].get<string>());
        }
        if (configJ.contains("gameType")) {
            params.gameType = gameTypeFromString(configJ["gameType"].get<string>());
        }
        if (configJ.contains("outputDir")) {
            params.outputDir = StringUtil::utf8toUTF16(configJ["outputDir"].get<string>());
        }
        if (configJ.contains("esmMode")) {
            params.esmMode = configJ["esmMode"].get<int>();
        }
        if (configJ.contains("meshBlockList")) {
            for (const auto& item : configJ["meshBlockList"]) {
                params.meshBlockList.push_back(StringUtil::utf8toUTF16(item.get<string>()));
            }
        }
        if (configJ.contains("seasonLockedEditorIDKeywords")) {
            params.seasonLockedEditorIDKeywords.clear();
            for (const auto& item : configJ["seasonLockedEditorIDKeywords"]) {
                params.seasonLockedEditorIDKeywords.push_back(StringUtil::utf8toUTF16(item.get<string>()));
            }
        }
        if (configJ.contains("removeGrassInWinter")) {
            params.removeGrassInWinter = configJ["removeGrassInWinter"].get<bool>();
        }
    } catch (const exception& e) {
        Logger::warn("Failed to parse config file, using defaults: {}", e.what());
        return ASParams {};
    }

    return params;
}

void ASConfig::save(const filesystem::path& exeDir, const ASParams& params)
{
    nlohmann::json configJ;

    configJ["uiLanguage"] = params.uiLanguage;
    configJ["gameDir"] = StringUtil::utf16toUTF8(params.gameDir.wstring());
    configJ["gameType"] = gameTypeToString(params.gameType);
    configJ["outputDir"] = StringUtil::utf16toUTF8(params.outputDir.wstring());
    configJ["esmMode"] = params.esmMode;
    configJ["meshBlockList"] = nlohmann::json::array();
    for (const auto& item : params.meshBlockList) {
        configJ["meshBlockList"].push_back(StringUtil::utf16toUTF8(item));
    }
    configJ["seasonLockedEditorIDKeywords"] = nlohmann::json::array();
    for (const auto& item : params.seasonLockedEditorIDKeywords) {
        configJ["seasonLockedEditorIDKeywords"].push_back(StringUtil::utf16toUTF8(item));
    }
    configJ["removeGrassInWinter"] = params.removeGrassInWinter;

    ofstream file(getConfigPath(exeDir));
    if (!file.is_open()) {
        Logger::warn("Failed to save config file");
        return;
    }

    file << configJ.dump(2);
}
