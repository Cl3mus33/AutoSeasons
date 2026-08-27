#include "GUI/SeasonModOverrideDialog.hpp"

#include "SeasonPatcher.hpp"

#include <boost/algorithm/string.hpp>
#include <wx/statline.h>

#include <algorithm>
#include <utility>

using namespace std;

namespace {
constexpr int BORDER_SIZE = 5;
}

SeasonModOverrideDialog::SeasonModOverrideDialog(wxWindow* parent, filesystem::path gameDir,
    BethesdaGame::GameType gameType, const std::vector<std::wstring>& currentOverrides,
    const std::unordered_map<std::string, std::vector<std::wstring>>& currentOverridesByType,
    const std::vector<std::wstring>& currentPriority,
    const std::unordered_map<std::string, std::vector<std::wstring>>& currentPriorityByType)
    : wxDialog(parent, wxID_ANY, ASTr("seasonModOverrides.title", "Season Mod Conflicts"), wxDefaultPosition,
          wxSize(480, 420), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_gameDir(std::move(gameDir))
    , m_gameType(gameType)
    , m_initialOverrides(currentOverrides)
    , m_initialPriority(currentPriority)
    , m_initialOverridesByType(currentOverridesByType)
    , m_initialPriorityByType(currentPriorityByType)
    , m_priorityOrderChanged(!currentPriority.empty())
{
    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    auto* introText = new wxStaticText(this, wxID_ANY,
        ASTr("seasonModOverrides.intro",
            "AutoSeasons normally defers to another mod's own Data/Seasons ini whenever it already "
            "covers a record for a season - every mod below starts checked (respected). Uncheck a "
            "mod to have AutoSeasons overwrite it instead: it generates its own seasonal texture "
            "wherever it has the art, dropping that mod's own declaration entirely. When two "
            "respected mods both cover the same record, the one lower in the list wins (like Mod "
            "Organizer's own load order)."));
    introText->Wrap(440);
    mainSizer->Add(introText, 0, wxALL, BORDER_SIZE * 2);

    // Called out separately (not just folded into the paragraph above) since it's the single
    // easiest thing to get burned by here: unchecking a mod isn't guaranteed to swap in
    // AutoSeasons' own art - if AutoSeasons has none for a given record, that record loses its
    // seasonal swap entirely instead of keeping the unchecked mod's own.
    auto* noArtWarning = new wxStaticText(this, wxID_ANY,
        ASTr("seasonModOverrides.noArtWarning",
            "Unchecking a mod removes its swap for a record AutoSeasons has no seasonal art of its "
            "own for - it does NOT fall back to keeping that mod's swap."));
    noArtWarning->SetForegroundColour(wxColour(180, 95, 0)); // amber - matches the launcher's own MO2/Vortex warning
    noArtWarning->Wrap(440);
    mainSizer->Add(noArtWarning, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    auto* scanButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.scanButton", "Scan"));
    scanButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onScanButtonPressed, this);
    mainSizer->Add(scanButton, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    // Quick-reference legend, distinct from the fuller explanation above - this list's checkbox and
    // its position each mean something different (unlike a typical MO2/Vortex load-order checklist,
    // where checking usually just means "enabled"), and that's easy to misread at a glance.
    auto* modListLegend = new wxStaticText(this, wxID_ANY,
        ASTr("seasonModOverrides.legend",
            "Checked = keep this mod's own coverage   |   Unchecked = overwrite it with AutoSeasons' own texture   |   "
            "Position = priority order (bottom wins)"));
    wxFont legendFont = modListLegend->GetFont();
    legendFont.SetStyle(wxFONTSTYLE_ITALIC);
    modListLegend->SetFont(legendFont);
    mainSizer->Add(modListLegend, 0, wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    auto* modListSizer = new wxBoxSizer(wxHORIZONTAL);
    m_modList = new wxCheckListBox(this, wxID_ANY);
    modListSizer->Add(m_modList, 1, wxEXPAND);

    // "To Top"/"To Bottom" sit outside the Up/Down pair (with a visual gap between them) since a
    // real load order can have 50+ entries - reaching either end one row at a time via Up/Down
    // alone doesn't scale.
    auto* moveButtonSizer = new wxBoxSizer(wxVERTICAL);
    m_moveTopButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveTop", "Move to Top"));
    m_moveTopButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveTopPressed, this);
    m_moveUpButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveUp", "Move Up"));
    m_moveUpButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveUpPressed, this);
    m_moveDownButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveDown", "Move Down"));
    m_moveDownButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveDownPressed, this);
    m_moveBottomButton = new wxButton(this, wxID_ANY, ASTr("seasonModOverrides.moveBottom", "Move to Bottom"));
    m_moveBottomButton->Bind(wxEVT_BUTTON, &SeasonModOverrideDialog::onMoveBottomPressed, this);
    moveButtonSizer->Add(m_moveTopButton, 0, wxBOTTOM | wxLEFT, BORDER_SIZE);
    moveButtonSizer->Add(m_moveUpButton, 0, wxBOTTOM | wxLEFT, BORDER_SIZE * 2);
    moveButtonSizer->Add(m_moveDownButton, 0, wxBOTTOM | wxLEFT, BORDER_SIZE * 2);
    moveButtonSizer->Add(m_moveBottomButton, 0, wxLEFT, BORDER_SIZE);
    modListSizer->Add(moveButtonSizer, 0, wxALIGN_TOP);

    mainSizer->Add(modListSizer, 1, wxEXPAND | wxLEFT | wxRIGHT, BORDER_SIZE * 2);

    m_statusText = new wxStaticText(this, wxID_ANY, wxEmptyString);
    mainSizer->Add(m_statusText, 0, wxALL, BORDER_SIZE * 2);

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
        return;
    }

    if (mods.empty()) {
        m_statusText->SetLabel(
            ASTr("seasonModOverrides.noModsFound", "No foreign season mods detected under Data/Seasons."));
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
        // Checked = respected (kept), unchecked = overwritten - the inverse of m_initialOverrides'
        // own "list of overwritten mods" storage, so a mod starts checked unless it was overwritten.
        if (!wasOverridden) {
            m_modList->Check(static_cast<unsigned int>(index), true);
        }
    }
}

void SeasonModOverrideDialog::onScanButtonPressed([[maybe_unused]] wxCommandEvent& event)
{
    runScan();
}

void SeasonModOverrideDialog::moveSelectedTo(unsigned int newIndex)
{
    const int sel = m_modList->GetSelection();
    if (sel == wxNOT_FOUND) {
        return;
    }
    const auto idx = static_cast<unsigned int>(sel);
    if (idx == newIndex || newIndex >= m_modList->GetCount()) {
        return;
    }

    const wxString text = m_modList->GetString(idx);
    const bool checked = m_modList->IsChecked(idx);
    m_modList->Delete(idx);
    m_modList->Insert(text, newIndex);
    m_modList->Check(newIndex, checked);
    m_modList->SetSelection(static_cast<int>(newIndex));

    const auto name = m_modNames.at(idx);
    m_modNames.erase(m_modNames.begin() + idx);
    m_modNames.insert(m_modNames.begin() + newIndex, name);

    m_priorityOrderChanged = true;
}

void SeasonModOverrideDialog::onMoveUpPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_modList->GetSelection();
    if (sel == wxNOT_FOUND || sel == 0) {
        return;
    }
    moveSelectedTo(static_cast<unsigned int>(sel) - 1);
}

void SeasonModOverrideDialog::onMoveDownPressed([[maybe_unused]] wxCommandEvent& event)
{
    const int sel = m_modList->GetSelection();
    if (sel == wxNOT_FOUND || static_cast<unsigned int>(sel) + 1 >= m_modList->GetCount()) {
        return;
    }
    moveSelectedTo(static_cast<unsigned int>(sel) + 1);
}

void SeasonModOverrideDialog::onMoveTopPressed([[maybe_unused]] wxCommandEvent& event)
{
    moveSelectedTo(0);
}

void SeasonModOverrideDialog::onMoveBottomPressed([[maybe_unused]] wxCommandEvent& event)
{
    if (m_modList->GetCount() == 0) {
        return;
    }
    moveSelectedTo(m_modList->GetCount() - 1);
}

auto SeasonModOverrideDialog::getSelectedOverrides() const -> std::vector<std::wstring>
{
    // Unchecked = overwritten (checked = respected/kept) - see m_modList's own inversion.
    vector<wstring> result;
    for (size_t i = 0; i < m_modNames.size(); i++) {
        if (!m_modList->IsChecked(static_cast<unsigned int>(i))) {
            result.push_back(m_modNames.at(i));
        }
    }
    return result;
}

auto SeasonModOverrideDialog::getPriorityOrder() const -> std::vector<std::wstring>
{
    // Only commit an order the user actually chose - opening this dialog, scanning, and clicking OK
    // without ever touching the order would otherwise silently persist whatever arbitrary order the
    // scan happened to produce, permanently ranking every currently-installed mod against any mod
    // installed later. See m_priorityOrderChanged's own doc comment.
    return m_priorityOrderChanged ? m_modNames : std::vector<std::wstring> {};
}

auto SeasonModOverrideDialog::getOverridesByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>
{
    return m_initialOverridesByType;
}

auto SeasonModOverrideDialog::getPriorityByType() const -> std::unordered_map<std::string, std::vector<std::wstring>>
{
    return m_initialPriorityByType;
}
