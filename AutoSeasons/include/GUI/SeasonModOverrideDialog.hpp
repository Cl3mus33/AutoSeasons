#pragma once

#include "ASLocale.hpp"
#include "common/BethesdaGame.hpp"

#include <wx/wx.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief Lets the user manage how AutoSeasons resolves conflicts between foreign mods' own
 * Data/Seasons coverage, via one single list: every mod starts CHECKED (respected/kept) -
 * unchecking one tells AutoSeasons to overwrite it instead (its own seasonal texture wins wherever
 * it has the art, and that mod's own declaration is dropped from the merged output entirely, so a
 * record AutoSeasons has no art for falls back to no swap rather than silently keeping the
 * overwritten mod's own swap). Position in the list sets cross-mod priority for when two or more
 * (non-overwritten) foreign mods both declare a swap for the very same base record - the mod lower
 * in the list wins, matching Mod Organizer's own left-pane priority convention (bottom = wins).
 * Applies uniformly across every record type - a genuinely rare need to differ by type is still
 * supported by the underlying engine/config file (overrideForeignSeasonModsByType/
 * foreignSeasonModPriorityByType in AutoSeasons_config.json) but intentionally has no dedicated
 * GUI here; a per-type customization dialog turned out to add real interface complexity for a need
 * that, in practice, was almost never used differently from the global choice.
 * Scans the current game location for every distinct foreign season mod (via
 * SeasonPatcher::discoverForeignSeasonMods()). Shown modally; on OK, getSelectedOverrides()/
 * getPriorityOrder() return the edited global settings, and getOverridesByType()/
 * getPriorityByType() pass through whatever per-type settings were already saved, unchanged.
 */
class SeasonModOverrideDialog : public wxDialog {
public:
    SeasonModOverrideDialog(wxWindow* parent, std::filesystem::path gameDir, BethesdaGame::GameType gameType,
        const std::vector<std::wstring>& currentOverrides,
        const std::unordered_map<std::string, std::vector<std::wstring>>& currentOverridesByType,
        const std::vector<std::wstring>& currentPriority,
        const std::unordered_map<std::string, std::vector<std::wstring>>& currentPriorityByType);

    [[nodiscard]] auto getSelectedOverrides() const -> std::vector<std::wstring>;
    /// @brief The global cross-mod priority order, top (lowest) to bottom (highest, wins).
    [[nodiscard]] auto getPriorityOrder() const -> std::vector<std::wstring>;
    /// @brief Pass-through of whatever per-type overrides were already saved - this dialog no
    /// longer edits them, but preserves them rather than silently wiping them out.
    [[nodiscard]] auto getOverridesByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>;
    /// @brief Pass-through of whatever per-type priority order was already saved - see getOverridesByType().
    [[nodiscard]] auto getPriorityByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>;

private:
    std::filesystem::path m_gameDir;
    BethesdaGame::GameType m_gameType;
    std::vector<std::wstring> m_initialOverrides;
    std::vector<std::wstring> m_initialPriority;
    std::unordered_map<std::string, std::vector<std::wstring>> m_initialOverridesByType;
    std::unordered_map<std::string, std::vector<std::wstring>> m_initialPriorityByType;

    std::vector<std::wstring> m_modNames; ///< Index-matched to m_modList's rows, current global order.
    /// @brief True once the user has actually reordered m_modList (Move Up/Down), or a global
    /// priority order already existed when the dialog opened. Guards getPriorityOrder(): opening
    /// this dialog, scanning, and clicking OK without ever touching the order must NOT silently
    /// commit an arbitrary (freshly-scanned) order as if the user had deliberately chosen it.
    bool m_priorityOrderChanged = false;

    wxCheckListBox* m_modList;
    wxButton* m_moveUpButton;
    wxButton* m_moveDownButton;
    wxStaticText* m_statusText;

    void runScan();

    void onScanButtonPressed(wxCommandEvent& event);
    void onMoveUpPressed(wxCommandEvent& event);
    void onMoveDownPressed(wxCommandEvent& event);
};
