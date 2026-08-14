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

auto ASConfig::load(const filesystem::path& exeDir) -> ASParams { return loadFrom(getConfigPath(exeDir)); }

void ASConfig::save(const filesystem::path& exeDir, const ASParams& params) { saveTo(getConfigPath(exeDir), params); }

auto ASConfig::loadFrom(const filesystem::path& configFilePath) -> ASParams
{
    ASParams params;

    nlohmann::json configJ;
    if (!FileUtil::getJSON(configFilePath, configJ)) {
        return params;
    }

    try {
        if (configJ.contains("uiLanguage")) {
            params.uiLanguage = configJ["uiLanguage"].get<string>();
        }
        if (configJ.contains("uiTheme")) {
            params.uiTheme = configJ["uiTheme"].get<string>();
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
            params.meshBlockList.clear();
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
        if (configJ.contains("overrideForeignSeasonMods")) {
            params.overrideForeignSeasonMods.clear();
            for (const auto& item : configJ["overrideForeignSeasonMods"]) {
                params.overrideForeignSeasonMods.push_back(StringUtil::utf8toUTF16(item.get<string>()));
            }
        }
        if (configJ.contains("overrideForeignSeasonModsByType")) {
            params.overrideForeignSeasonModsByType.clear();
            for (const auto& [typeKey, arr] : configJ["overrideForeignSeasonModsByType"].items()) {
                vector<wstring> list;
                for (const auto& item : arr) {
                    list.push_back(StringUtil::utf8toUTF16(item.get<string>()));
                }
                params.overrideForeignSeasonModsByType[typeKey] = std::move(list);
            }
        }
        if (configJ.contains("foreignSeasonModPriority")) {
            params.foreignSeasonModPriority.clear();
            for (const auto& item : configJ["foreignSeasonModPriority"]) {
                params.foreignSeasonModPriority.push_back(StringUtil::utf8toUTF16(item.get<string>()));
            }
        }
        if (configJ.contains("foreignSeasonModPriorityByType")) {
            params.foreignSeasonModPriorityByType.clear();
            for (const auto& [typeKey, arr] : configJ["foreignSeasonModPriorityByType"].items()) {
                vector<wstring> list;
                for (const auto& item : arr) {
                    list.push_back(StringUtil::utf8toUTF16(item.get<string>()));
                }
                params.foreignSeasonModPriorityByType[typeKey] = std::move(list);
            }
        }
    } catch (const exception& e) {
        Logger::warn("Failed to parse config file, using defaults: {}", e.what());
        return ASParams {};
    }

    return params;
}

void ASConfig::saveTo(const filesystem::path& configFilePath, const ASParams& params)
{
    nlohmann::json configJ;

    configJ["uiLanguage"] = params.uiLanguage;
    configJ["uiTheme"] = params.uiTheme;
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
    configJ["overrideForeignSeasonMods"] = nlohmann::json::array();
    for (const auto& item : params.overrideForeignSeasonMods) {
        configJ["overrideForeignSeasonMods"].push_back(StringUtil::utf16toUTF8(item));
    }
    configJ["overrideForeignSeasonModsByType"] = nlohmann::json::object();
    for (const auto& [typeKey, list] : params.overrideForeignSeasonModsByType) {
        auto arr = nlohmann::json::array();
        for (const auto& item : list) {
            arr.push_back(StringUtil::utf16toUTF8(item));
        }
        configJ["overrideForeignSeasonModsByType"][typeKey] = arr;
    }
    configJ["foreignSeasonModPriority"] = nlohmann::json::array();
    for (const auto& item : params.foreignSeasonModPriority) {
        configJ["foreignSeasonModPriority"].push_back(StringUtil::utf16toUTF8(item));
    }
    configJ["foreignSeasonModPriorityByType"] = nlohmann::json::object();
    for (const auto& [typeKey, list] : params.foreignSeasonModPriorityByType) {
        auto arr = nlohmann::json::array();
        for (const auto& item : list) {
            arr.push_back(StringUtil::utf16toUTF8(item));
        }
        configJ["foreignSeasonModPriorityByType"][typeKey] = arr;
    }

    ofstream file(configFilePath);
    if (!file.is_open()) {
        Logger::warn("Failed to save config file");
        return;
    }

    file << configJ.dump(2);
}
