#include "GUI/SeasonModOverrideDialog.hpp"

#include "SeasonPatcher.hpp"

#include <boost/algorithm/string.hpp>
#include <wx/statline.h>

#include <algorithm>
#include <array>
#include <utility>

using namespace std;

namespace {
constexpr int BORDER_SIZE = 5;

// (display label, SeasonPatcher record-type key) - order matches the wxChoice's own item order,
// so m_typeChoice's selection index can be used directly to look this up.
constexpr array<pair<const char*, const char*>, 7> RECORD_TYPES { {
    { "Statics (STAT)", "STAT" },
    { "Landscape Textures (LTEX)", "LTEX" },
    { "Activators (ACTI)", "ACTI" },
    { "Furniture (FURN)", "FURN" },
    { "Moveable Statics (MSTT)", "MSTT" },
    { "Trees (TREE)", "TREE" },
    { "Flora (FLOR)", "FLOR" },
} };
}

SeasonModOverrideDialog::SeasonModOverrideDialog(wxWindow* parent, filesystem::path gameDir,
    BethesdaGame::GameType gameType, const std::vector<std::wstring>& currentOverrides,
    const std::unordered_map<std::string, std::vector<std::wstring>>& currentOverridesByType,
    const std::vector<std::wstring>& currentPriority,
    const std::unordered_map<std::string, std::vector<std::wstring>>& currentPriorityByType)
    : wxDialog(parent, wxID_ANY, ASTr("seasonModOverrides.title", "Season Mod Conflicts"), wxDefaultPosition,
          wxSize(480, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_gameDir(std::move(gameDir))
    , m_gameType(gameType)
    , m_initialOverrides(currentOverrides)
    , m_initialPriority(currentPriority)
    , m_priorityByType(currentPriorityByType)
    , m_overridesByType(currentOverridesByType)
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    auto* introText = new wxStaticText(this, wxID_ANY,
        ASTr("seasonModOverrides.intro",
            "AutoSeasons normally defers to another mod's own Data/Seasons ini whenever it already "
            "covers a record for a season. Check a mod below to have AutoSeasons override it "
            "instead: it generates its own seasonal texture wherever it has the art, and that mod's "
            "own declaration is dropped entirely (so a record with no AutoSeasons art falls back to "
            "no swap, not the overridden mod's swap). When two non-overridden mods both cover the "
            "same record, the one lower in the list wins (like Mod Organizer's own load order)."));
    introText->Wrap(440);
    mainSizer->Add(introText, 0, wxALL, BORDER_SIZE * 2);

    auto* scanButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.scanButton", "Scan"));
    scanButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onScanButtonPressed, this);
    mainSizer->Add(scanButton, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    auto* modListSizer = new wxBoxSizer(wxHORIZONTAL);
    m_modList = new wxCheckListBox(this, wxID_ANY);
    modListSizer->Add(m_modList, 1, wxEXPAND);

    auto* moveButtonSizer = new wxBoxSizer(wxVERTICAL);
    m_moveUpButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveUp", "Move Up"));
    m_moveUpButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveUpPressed, this);
    m_moveDownButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveDown", "Move Down"));
    m_moveDownButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveDownPressed, this);
    moveButtonSizer->Add(m_moveUpButton, 0, wxBOTTOM | wxLEFT, BORDER_SIZE);
    moveButtonSizer->Add(m_moveDownButton, 0, wxLEFT, BORDER_SIZE);
    modListSizer->Add(moveButtonSizer, 0, wxALIGN_TOP);

    mainSizer->Add(modListSizer, 2, wxEXPAND | wxLEFT | wxRIGHT, BORDER_SIZE * 2);

    m_statusText = new wxStaticText(this, wxID_ANY, wxEmptyString);
    mainSizer->Add(m_statusText, 0, wxALL, BORDER_SIZE * 2);

    mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxALL, BORDER_SIZE);

    auto* typeIntroText = new wxStaticText(this, wxID_ANY,
        ASTr("seasonModOverrides.typeIntro",
            "Optional: use different override/priority choices for one specific record type (e.g. "
            "override a mod, or prefer a different one, just for trees without touching every other "
            "type)."));
    typeIntroText->Wrap(440);
    mainSizer->Add(typeIntroText, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE * 2);

    auto* typeChoiceSizer = new wxBoxSizer(wxHORIZONTAL);
    wxArrayString typeChoices;
    for (const auto& [label, key] : RECORD_TYPES) {
        typeChoices.Add(label);
    }
    m_typeChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, typeChoices);
    m_typeChoice->SetSelection(0);
    m_typeChoice->Bind(wxEVT_CHOICE, &SeasonModOverrideDialog::onTypeChoiceChanged, this);
    typeChoiceSizer->Add(m_typeChoice, 0, wxALL, BORDER_SIZE);

    m_customizeTypeCheckbox = new wxCheckBox(
        this, wxID_ANY, ASTr("seasonModOverrides.customizeType", "Customize overrides/order for this type"));
    m_customizeTypeCheckbox->Bind(wxEVT_CHECKBOX, &SeasonModOverrideDialog::onCustomizeTypeToggled, this);
    typeChoiceSizer->Add(m_customizeTypeCheckbox, 0, wxALL | wxALIGN_CENTER_VERTICAL, BORDER_SIZE);

    mainSizer->Add(typeChoiceSizer, 0, wxLEFT | wxRIGHT, BORDER_SIZE);

    auto* typeListSizer = new wxBoxSizer(wxHORIZONTAL);
    m_typeOrderList = new wxCheckListBox(this, wxID_ANY);
    m_typeOrderList->Bind(wxEVT_CHECKLISTBOX, &SeasonModOverrideDialog::onTypeListToggled, this);
    typeListSizer->Add(m_typeOrderList, 1, wxEXPAND);

    auto* typeMoveButtonSizer = new wxBoxSizer(wxVERTICAL);
    m_typeMoveUpButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveUp", "Move Up"));
    m_typeMoveUpButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onTypeMoveUpPressed, this);
    m_typeMoveDownButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveDown", "Move Down"));
    m_typeMoveDownButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onTypeMoveDownPressed, this);
    typeMoveButtonSizer->Add(m_typeMoveUpButton, 0, wxBOTTOM | wxLEFT, BORDER_SIZE);
    typeMoveButtonSizer->Add(m_typeMoveDownButton, 0, wxLEFT, BORDER_SIZE);
    typeListSizer->Add(typeMoveButtonSizer, 0, wxALIGN_TOP);

    mainSizer->Add(typeListSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* cancelButton = new wxButton(this, wxID_CANCEL, ASTr("common.cancel", "Cancel"));
    auto* okButton = new wxButton(this, wxID_OK, ASTr("common.ok", "OK"));
    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(cancelButton, 0, wxALL, BORDER_SIZE);
    buttonSizer->Add(okButton, 0, wxALL, BORDER_SIZE);
    mainSizer->Add(buttonSizer, 0, wxEXPAND);

    SetSizerAndFit(mainSizer);

    if (!m_gameDir.empty()) {
        runScan();
    } else {
        populateTypeOrderList();
    }
}

void SeasonModOverrideDialog::runScan()
{
    m_modList->Clear();
    m_modNames.clear();

    vector<SeasonPatcher::ForeignSeasonModInfo> mods;
    try {
        const auto bg = BethesdaGame(m_gameType, m_gameDir);
        mods = SeasonPatcher::discoverForeignSeasonMods(bg.getGameDataPath());
    } catch (const exception& e) {
        m_statusText->SetLabel(ASTr("seasonModOverrides.scanError", "Could not scan: ") + wxString(e.what()));
        populateTypeOrderList();
        return;
    }

    if (mods.empty()) {
        m_statusText->SetLabel(
            ASTr("seasonModOverrides.noModsFound", "No foreign season mods detected under Data/Seasons."));
        populateTypeOrderList();
        return;
    }

    m_statusText->SetLabel(wxEmptyString);

    // Apply the previously saved global priority order (mods present in m_initialPriority keep
    // that relative order and sort after anything not present - mirrors run()'s own "unranked =
    // lowest" rule), so reopening this dialog shows exactly the order that was actually used.
    unordered_map<wstring, size_t> rankByMod;
    for (size_t i = 0; i < m_initialPriority.size(); i++) {
        rankByMod[boost::algorithm::to_lower_copy(m_initialPriority[i])] = i;
    }
    ranges::stable_sort(mods, {}, [&](const SeasonPatcher::ForeignSeasonModInfo& m) -> long long {
        const auto it = rankByMod.find(boost::algorithm::to_lower_copy(m.modName));
        return it != rankByMod.end() ? static_cast<long long>(it->second) : -1;
    });

    for (const auto& mod : mods) {
        m_modNames.push_back(mod.modName);
        const auto index = m_modList->Append(wxString::Format("%s (%zu)", wxString(mod.modName), mod.entryCount));
        const auto modNameLower = boost::algorithm::to_lower_copy(mod.modName);
        const bool wasOverridden = std::ranges::any_of(m_initialOverrides, [&](const auto& name) {
            return boost::algorithm::to_lower_copy(name) == modNameLower;
        });
        if (wasOverridden) {
            m_modList->Check(static_cast<unsigned int>(index), true);
        }
    }

    populateTypeOrderList();
}

auto SeasonModOverrideDialog::getCurrentTypeKey() const -> std::string
{
    const int sel = m_typeChoice->GetSelection();
    if (sel == wxNOT_FOUND || sel < 0 || static_cast<size_t>(sel) >= RECORD_TYPES.size()) {
        return RECORD_TYPES.front().second;
    }
    return RECORD_TYPES.at(static_cast<size_t>(sel)).second;
}

void SeasonModOverrideDialog::populateTypeOrderList()
{
    const auto typeKey = getCurrentTypeKey();
    m_typeOrderList->Clear();

    const auto orderIt = m_priorityByType.find(typeKey);
    const bool customized = orderIt != m_priorityByType.end();
    m_customizeTypeCheckbox->SetValue(customized);

    const auto& overridesForType = m_overridesByType[typeKey]; // empty vector if not customized yet - fine, just no checks below

    if (customized) {
        for (const auto& name : orderIt->second) {
            const auto index = m_typeOrderList->Append(wxString(name));
            const auto nameLower = boost::algorithm::to_lower_copy(name);
            const bool checked = std::ranges::any_of(overridesForType, [&](const auto& overriddenName) {
                return boost::algorithm::to_lower_copy(overriddenName) == nameLower;
            });
            if (checked) {
                m_typeOrderList->Check(static_cast<unsigned int>(index), true);
            }
        }
    } else {
        // Not customized - preview the current global list (order AND checked state) as the
        // starting point a user would get if they turned customization on right now.
        for (size_t i = 0; i < m_modNames.size(); i++) {
            const auto index = m_typeOrderList->Append(wxString(m_modNames.at(i)));
            if (m_modList->IsChecked(static_cast<unsigned int>(i))) {
                m_typeOrderList->Check(static_cast<unsigned int>(index), true);
            }
        }
    }

    updateTypeControlsEnabled();
}

void SeasonModOverrideDialog::updateTypeControlsEnabled()
{
    const bool enabled = m_customizeTypeCheckbox->GetValue();
    m_typeOrderList->Enable(enabled);
    m_typeMoveUpButton->Enable(enabled);
    m_typeMoveDownButton->Enable(enabled);
}

void SeasonModOverrideDialog::saveTypeListState()
{
    const auto typeKey = getCurrentTypeKey();

    vector<wstring> order;
    vector<wstring> overridden;
    for (unsigned int i = 0; i < m_typeOrderList->GetCount(); i++) {
        const auto name = m_typeOrderList->GetString(i).ToStdWstring();
        order.push_back(name);
        if (m_typeOrderList->IsChecked(i)) {
            overridden.push_back(name);
        }
    }
    m_priorityByType[typeKey] = std::move(order);
    m_overridesByType[typeKey] = std::move(overridden);
}

void SeasonModOverrideDialog::onScanButtonPressed([[maybe_unused]] wxCommandEvent& event)
{
    runScan();
}

void SeasonModOverrideDialog::onMoveUpPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_modList->GetSelection();
    if (sel == wxNOT_FOUND || sel == 0) {
        return;
    }
    const auto idx = static_cast<unsigned int>(sel);
    const wxString text = m_modList->GetString(idx);
    const bool checked = m_modList->IsChecked(idx);
    m_modList->Delete(idx);
    m_modList->Insert(text, idx - 1);
    m_modList->Check(idx - 1, checked);
    m_modList->SetSelection(static_cast<int>(idx - 1));
    std::swap(m_modNames.at(idx), m_modNames.at(idx - 1));
}

void SeasonModOverrideDialog::onMoveDownPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_modList->GetSelection();
    if (sel == wxNOT_FOUND || static_cast<unsigned int>(sel) + 1 >= m_modList->GetCount()) {
        return;
    }
    const auto idx = static_cast<unsigned int>(sel);
    const wxString text = m_modList->GetString(idx);
    const bool checked = m_modList->IsChecked(idx);
    m_modList->Delete(idx);
    m_modList->Insert(text, idx + 1);
    m_modList->Check(idx + 1, checked);
    m_modList->SetSelection(static_cast<int>(idx + 1));
    std::swap(m_modNames.at(idx), m_modNames.at(idx + 1));
}

void SeasonModOverrideDialog::onTypeChoiceChanged([[maybe_unused]] wxCommandEvent& event)
{
    populateTypeOrderList();
}

void SeasonModOverrideDialog::onCustomizeTypeToggled([[maybe_unused]] wxCommandEvent& event)
{
    const auto typeKey = getCurrentTypeKey();
    if (m_customizeTypeCheckbox->GetValue()) {
        // Just turned on - commit whatever the list is currently previewing (a snapshot of the
        // global order/overrides) as this type's own, now-independent customization.
        saveTypeListState();
        updateTypeControlsEnabled();
    } else {
        m_priorityByType.erase(typeKey);
        m_overridesByType.erase(typeKey);
        populateTypeOrderList(); // refresh back to previewing the (current) global list
    }
}

void SeasonModOverrideDialog::onTypeListToggled([[maybe_unused]] wxCommandEvent& event)
{
    // A checkbox was clicked while previewing the (not-yet-customized) global list - clicking it
    // is itself the signal that the user wants a per-type override, so turn customization on
    // rather than silently discarding the click when populateTypeOrderList() next runs.
    if (!m_customizeTypeCheckbox->GetValue()) {
        m_customizeTypeCheckbox->SetValue(true);
        updateTypeControlsEnabled();
    }
    saveTypeListState();
}

void SeasonModOverrideDialog::onTypeMoveUpPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_typeOrderList->GetSelection();
    if (sel == wxNOT_FOUND || sel == 0) {
        return;
    }
    const auto idx = static_cast<unsigned int>(sel);
    const wxString text = m_typeOrderList->GetString(idx);
    const bool checked = m_typeOrderList->IsChecked(idx);
    m_typeOrderList->Delete(idx);
    m_typeOrderList->Insert(text, idx - 1);
    m_typeOrderList->Check(idx - 1, checked);
    m_typeOrderList->SetSelection(static_cast<int>(idx - 1));
    saveTypeListState();
}

void SeasonModOverrideDialog::onTypeMoveDownPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_typeOrderList->GetSelection();
    if (sel == wxNOT_FOUND || static_cast<unsigned int>(sel) + 1 >= m_typeOrderList->GetCount()) {
        return;
    }
    const auto idx = static_cast<unsigned int>(sel);
    const wxString text = m_typeOrderList->GetString(idx);
    const bool checked = m_typeOrderList->IsChecked(idx);
    m_typeOrderList->Delete(idx);
    m_typeOrderList->Insert(text, idx + 1);
    m_typeOrderList->Check(idx + 1, checked);
    m_typeOrderList->SetSelection(static_cast<int>(idx + 1));
    saveTypeListState();
}

auto SeasonModOverrideDialog::getSelectedOverrides() const -> std::vector<std::wstring>
{
    vector<wstring> result;
    for (size_t i = 0; i < m_modNames.size(); i++) {
        if (m_modList->IsChecked(static_cast<unsigned int>(i))) {
            result.push_back(m_modNames.at(i));
        }
    }
    return result;
}

auto SeasonModOverrideDialog::getPriorityOrder() const -> std::vector<std::wstring>
{
    return m_modNames;
}

auto SeasonModOverrideDialog::getOverridesByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>
{
    return m_overridesByType;
}

auto SeasonModOverrideDialog::getPriorityByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>
{
    return m_priorityByType;
}
