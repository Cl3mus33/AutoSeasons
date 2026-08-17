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
 * Data/Seasons coverage. Every mod in the list starts CHECKED (respected/kept) - unchecking one
 * tells AutoSeasons to overwrite it instead:
 *  - which mods (if any) AutoSeasons should overwrite entirely - AutoSeasons generates its own
 *    seasonal duplicate wherever it has the art, AND that mod's own raw ini line is dropped from
 *    the merged output entirely (so a record AutoSeasons has no art for falls back to no swap,
 *    rather than silently keeping the overwritten mod's own swap);
 *  - a cross-mod priority order used when two or more (non-overwritten) foreign mods both declare
 *    a swap for the very same base record - the mod lower in the list wins, matching Mod
 *    Organizer's own left-pane priority convention (bottom = wins);
 *  - an optional per-record-type override of both the above (e.g. overwriting a mod, or preferring
 *    a different one, just for TREE records without touching every other type).
 * Scans the current game location for every distinct foreign season mod (via
 * SeasonPatcher::discoverForeignSeasonMods()). Shown modally; on OK, getSelectedOverrides()/
 * getPriorityOrder()/getOverridesByType()/getPriorityByType() return the edited settings (still
 * "list of overwritten mods", matching ASParams/AutoSeasons_config.json - the checkbox inversion
 * is purely a GUI-display concern, not a change to the underlying data model).
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
    [[nodiscard]] auto getOverridesByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>;
    [[nodiscard]] auto getPriorityByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>;

private:
    std::filesystem::path m_gameDir;
    BethesdaGame::GameType m_gameType;
    std::vector<std::wstring> m_initialOverrides;
    std::vector<std::wstring> m_initialPriority;
    /// @brief Working copy of the per-type order, edited live as the user customizes each type.
    std::unordered_map<std::string, std::vector<std::wstring>> m_priorityByType;
    /// @brief Working copy of the per-type overrides, edited live as the user customizes each type.
    std::unordered_map<std::string, std::vector<std::wstring>> m_overridesByType;

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

    wxChoice* m_typeChoice;
    wxCheckBox* m_customizeTypeCheckbox;
    wxCheckListBox* m_typeOrderList; ///< Same checkbox+order pattern as m_modList, scoped to one record type.
    wxButton* m_typeMoveUpButton;
    wxButton* m_typeMoveDownButton;

    void runScan();
    [[nodiscard]] auto getCurrentTypeKey() const -> std::string;
    void populateTypeOrderList();
    void updateTypeControlsEnabled();
    /// @brief Writes m_typeOrderList's current order+checked state back into m_priorityByType/m_overridesByType.
    void saveTypeListState();

    void onScanButtonPressed(wxCommandEvent& event);
    void onMoveUpPressed(wxCommandEvent& event);
    void onMoveDownPressed(wxCommandEvent& event);
    void onTypeChoiceChanged(wxCommandEvent& event);
    void onCustomizeTypeToggled(wxCommandEvent& event);
    void onTypeListToggled(wxCommandEvent& event);
    void onTypeMoveUpPressed(wxCommandEvent& event);
    void onTypeMoveDownPressed(wxCommandEvent& event);
};
