#include "GUI/LauncherWindow.hpp"

#include "GUI/components/PGCustomListctrlChangedEvent.hpp"
#include "common/BethesdaGame.hpp"
#include "util/StringUtil.hpp"

#include <wx/statline.h>

using namespace std;

namespace {
constexpr int BORDER_SIZE = 5;
}

LauncherWindow::LauncherWindow(const ASParams& initParams, filesystem::path exePath)
    : wxDialog(nullptr, wxID_ANY, "AutoSeasons", wxDefaultPosition, wxSize(560, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_exePath(std::move(exePath))
{
    const wxIcon appIcon(wxICON(IDI_ICON1));
    SetIcon(appIcon);

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Language selector - changing it immediately relaunches the window (see onLanguageChanged)
    // rather than requiring a separate settings dialog/OK click, since AutoSeasons' launcher is
    // small enough that a full rebuild is cheap and this keeps the UX to a single click.
    auto* languageLabel = new wxStaticText(this, wxID_ANY, ASTr("launcher.language.label", "Language"));
    mainSizer->Add(languageLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_languages = ASLocale::getAvailableLanguages();
    wxArrayString languageChoices;
    int selectedLanguageIndex = 0;
    for (size_t i = 0; i < m_languages.size(); i++) {
        languageChoices.Add(m_languages.at(i).displayName);
        if (m_languages.at(i).code == ASLocale::getCurrentLanguage()) {
            selectedLanguageIndex = static_cast<int>(i);
        }
    }
    m_languageChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, languageChoices);
    if (!m_languages.empty()) {
        m_languageChoice->SetSelection(selectedLanguageIndex);
    }
    m_languageChoice->Bind(wxEVT_CHOICE, &LauncherWindow::onLanguageChanged, this);
    mainSizer->Add(m_languageChoice, 0, wxEXPAND | wxALL, BORDER_SIZE);

    auto* introText = new wxStaticText(this, wxID_ANY,
        ASTr("launcher.intro",
            "Scans your load order for seasonal texture variants (e.g. rock01_AUT.dds next to "
            "rock01.dds) and generates the plugin + Data/Seasons/*.ini files for the Seasons of "
            "Skyrim SKSE plugin."));
    introText->Wrap(490);
    mainSizer->Add(introText, 0, wxALL, BORDER_SIZE * 2);

    // Game location
    auto* gameLocationLabel = new wxStaticText(this, wxID_ANY, ASTr("launcher.gameLocation.label", "Game Location"));
    mainSizer->Add(gameLocationLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_gameLocationTextbox = new wxTextCtrl(this, wxID_ANY, initParams.gameDir.wstring());
    auto* gameBrowseButton = new wxButton(this, wxID_ANY, ASTr("common.browse", "Browse"));
    gameBrowseButton->Bind(wxEVT_BUTTON, &LauncherWindow::onBrowseGameLocation, this);

    auto* gameLocationSizer = new wxBoxSizer(wxHORIZONTAL);
    gameLocationSizer->Add(m_gameLocationTextbox, 1, wxEXPAND | wxALL, BORDER_SIZE);
    gameLocationSizer->Add(gameBrowseButton, 0, wxALL, BORDER_SIZE);
    mainSizer->Add(gameLocationSizer, 0, wxEXPAND);

    // Game type
    auto* gameTypeLabel = new wxStaticText(this, wxID_ANY, ASTr("launcher.gameType.label", "Game Type"));
    mainSizer->Add(gameTypeLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    wxArrayString gameTypeChoices;
    int selectedGameTypeIndex = 0;
    int curGameTypeIndex = 0;
    for (const auto& gameType : BethesdaGame::getGameTypes()) {
        gameTypeChoices.Add(BethesdaGame::getStrFromGameType(gameType));
        if (gameType == initParams.gameType) {
            selectedGameTypeIndex = curGameTypeIndex;
        }
        curGameTypeIndex++;
    }
    m_gameTypeChoice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, gameTypeChoices);
    m_gameTypeChoice->SetSelection(selectedGameTypeIndex);
    mainSizer->Add(m_gameTypeChoice, 0, wxEXPAND | wxALL, BORDER_SIZE);

    // Output location
    auto* outputLocationLabel = new wxStaticText(this, wxID_ANY, ASTr("launcher.outputLocation.label", "Output Location"));
    mainSizer->Add(outputLocationLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_outputLocationTextbox = new wxTextCtrl(this, wxID_ANY, initParams.outputDir.wstring());
    auto* outputBrowseButton = new wxButton(this, wxID_ANY, ASTr("common.browse", "Browse"));
    outputBrowseButton->Bind(wxEVT_BUTTON, &LauncherWindow::onBrowseOutputLocation, this);

    auto* outputLocationSizer = new wxBoxSizer(wxHORIZONTAL);
    outputLocationSizer->Add(m_outputLocationTextbox, 1, wxEXPAND | wxALL, BORDER_SIZE);
    outputLocationSizer->Add(outputBrowseButton, 0, wxALL, BORDER_SIZE);
    mainSizer->Add(outputLocationSizer, 0, wxEXPAND);

    // Mesh blocklist - an inline editable table (not a separate popup), so the current rules are
    // always visible at a glance. Right click, or double-click a row, to add/remove entries.
    auto* blocklistLabel = new wxStaticText(this, wxID_ANY, ASTr("launcher.blocklist.label", "Seasonal Variation Blocklist"));
    mainSizer->Add(blocklistLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    auto* blocklistHelpText = new wxStaticText(this, wxID_ANY,
        ASTr("launcher.blocklist.help",
            "Meshes matching a rule here are skipped by seasonal variation (useful for interior "
            "meshes, since Seasons of Skyrim only swaps records in exterior worldspaces). Wildcards "
            "(*) allowed, e.g. \"*/interiors/*\". Right click to add/remove rows."));
    blocklistHelpText->Wrap(520);
    mainSizer->Add(blocklistHelpText, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_blocklistCtrl = new PGModifiableListCtrl(
        this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_EDIT_LABELS | wxLC_NO_HEADER);
    m_blocklistCtrl->AppendColumn(ASTr("launcher.blocklist.column", "Rule"), wxLIST_FORMAT_LEFT, wxLIST_AUTOSIZE_USEHEADER);
    m_blocklistCtrl->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);

    long blocklistIndex = 0;
    for (const auto& rule : initParams.meshBlockList) {
        m_blocklistCtrl->InsertItem(blocklistIndex++, wxString(rule));
    }
    m_blocklistCtrl->InsertItem(m_blocklistCtrl->GetItemCount(), "");

    mainSizer->Add(m_blocklistCtrl, 1, wxEXPAND | wxALL, BORDER_SIZE);

    // Season-locked EditorID keywords - same inline editable table pattern as the mesh blocklist
    // above. Records whose EditorID contains one of these (case-insensitive substring match) are
    // skipped by seasonal variation entirely, e.g. so a mod's own "...Coast01"/"...River01"
    // statics never get a pointless seasonal duplicate regardless of which texture files exist.
    auto* editorIDKeywordsLabel
        = new wxStaticText(this, wxID_ANY, ASTr("launcher.editorIdKeywords.label", "Season-Locked EditorID Keywords"));
    mainSizer->Add(editorIDKeywordsLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    auto* editorIDKeywordsHelpText = new wxStaticText(this, wxID_ANY,
        ASTr("launcher.editorIdKeywords.help",
            "Records whose EditorID contains one of these words (case-insensitive) are skipped by "
            "seasonal variation entirely - useful for names that already signal a fixed "
            "season/biome, e.g. \"Coast\" or \"River\". Right click to add/remove rows."));
    editorIDKeywordsHelpText->Wrap(520);
    mainSizer->Add(editorIDKeywordsHelpText, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_editorIDKeywordsCtrl = new PGModifiableListCtrl(
        this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_EDIT_LABELS | wxLC_NO_HEADER);
    m_editorIDKeywordsCtrl->AppendColumn(
        ASTr("launcher.editorIdKeywords.column", "Keyword"), wxLIST_FORMAT_LEFT, wxLIST_AUTOSIZE_USEHEADER);
    m_editorIDKeywordsCtrl->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);

    long editorIDKeywordIndex = 0;
    for (const auto& keyword : initParams.seasonLockedEditorIDKeywords) {
        m_editorIDKeywordsCtrl->InsertItem(editorIDKeywordIndex++, wxString(keyword));
    }
    m_editorIDKeywordsCtrl->InsertItem(m_editorIDKeywordsCtrl->GetItemCount(), "");

    mainSizer->Add(m_editorIDKeywordsCtrl, 1, wxEXPAND | wxALL, BORDER_SIZE);

    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) -> void {
        updateListColumnWidths();
        event.Skip();
    });
    m_blocklistCtrl->Bind(pgEVT_LISTCTRL_CHANGED, [this](PGCustomListctrlChangedEvent& event) -> void {
        updateListColumnWidths();
        event.Skip();
    });
    m_editorIDKeywordsCtrl->Bind(pgEVT_LISTCTRL_CHANGED, [this](PGCustomListctrlChangedEvent& event) -> void {
        updateListColumnWidths();
        event.Skip();
    });

    // ESM flag: always on (matches PGPatcher's own output plugin convention). ESL-flagging
    // happens automatically and safely based on record count, independent of this - not worth
    // exposing as a separate choice, it was just a confusing extra decision.

    mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxALL, BORDER_SIZE);

    // Buttons
    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* cancelButton = new wxButton(this, wxID_CANCEL, ASTr("common.cancel", "Cancel"));
    m_okButton = new wxButton(this, wxID_ANY, ASTr("launcher.startButton", "Start Patching"));
    m_okButton->Bind(wxEVT_BUTTON, &LauncherWindow::onOkButtonPressed, this);
    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(cancelButton, 0, wxALL, BORDER_SIZE);
    buttonSizer->Add(m_okButton, 0, wxALL, BORDER_SIZE);
    mainSizer->Add(buttonSizer, 0, wxEXPAND);

    SetSizerAndFit(mainSizer);
}

void LauncherWindow::getParams(ASParams& outParams) const
{
    outParams.gameDir = m_gameLocationTextbox->GetValue().ToStdWstring();
    outParams.outputDir = m_outputLocationTextbox->GetValue().ToStdWstring();
    outParams.gameType = BethesdaGame::getGameTypes()[static_cast<size_t>(m_gameTypeChoice->GetSelection())];
    outParams.uiLanguage = ASLocale::getCurrentLanguage();
    // removeGrassInWinter has no GUI control - preserve whatever was loaded from
    // AutoSeasons_config.json (or the ASParams default) rather than overwriting it here.
    // Never ESM-flag: an ESM-flagged plugin historically could only have other ESM-flagged
    // plugins as masters, which would make depending on a regular ESP (e.g. TerrainHelper.esp,
    // for the LandscapeDefault parallax patch) unsafe. ESL-flagging (which allows depending on
    // any plugin regardless of its own flags) still happens automatically and independently.
    outParams.esmMode = 2;

    outParams.meshBlockList.clear();
    long item = -1;
    while ((item = m_blocklistCtrl->GetNextItem(item)) != -1) {
        const wxString text = m_blocklistCtrl->GetItemText(item);
        if (!text.IsEmpty()) {
            outParams.meshBlockList.push_back(text.ToStdWstring());
        }
    }

    outParams.seasonLockedEditorIDKeywords.clear();
    item = -1;
    while ((item = m_editorIDKeywordsCtrl->GetNextItem(item)) != -1) {
        const wxString text = m_editorIDKeywordsCtrl->GetItemText(item);
        if (!text.IsEmpty()) {
            outParams.seasonLockedEditorIDKeywords.push_back(text.ToStdWstring());
        }
    }
}

void LauncherWindow::onLanguageChanged([[maybe_unused]] wxCommandEvent& event)
{
    const int selection = m_languageChoice->GetSelection();
    if (selection == wxNOT_FOUND) {
        return;
    }

    const auto& selectedLang = m_languages.at(static_cast<size_t>(selection));
    if (selectedLang.code == ASLocale::getCurrentLanguage()) {
        return;
    }

    ASLocale::init(m_exePath / "translations", selectedLang.code);
    EndModal(RESULT_RELAUNCH);
}

void LauncherWindow::onBrowseGameLocation([[maybe_unused]] wxCommandEvent& event)
{
    wxDirDialog dialog(this, ASTr("launcher.gameLocation.dialogTitle", "Select Game Location"), m_gameLocationTextbox->GetValue());
    if (dialog.ShowModal() == wxID_OK) {
        m_gameLocationTextbox->SetValue(dialog.GetPath());
    }
}

void LauncherWindow::onBrowseOutputLocation([[maybe_unused]] wxCommandEvent& event)
{
    wxDirDialog dialog(
        this, ASTr("launcher.outputLocation.dialogTitle", "Select Output Location"), m_outputLocationTextbox->GetValue());
    if (dialog.ShowModal() == wxID_OK) {
        m_outputLocationTextbox->SetValue(dialog.GetPath());
    }
}

void LauncherWindow::updateListColumnWidths()
{
    if (m_blocklistCtrl != nullptr && m_blocklistCtrl->GetColumnCount() > 0) {
        m_blocklistCtrl->SetColumnWidth(0, m_blocklistCtrl->GetClientSize().GetWidth());
    }
    if (m_editorIDKeywordsCtrl != nullptr && m_editorIDKeywordsCtrl->GetColumnCount() > 0) {
        m_editorIDKeywordsCtrl->SetColumnWidth(0, m_editorIDKeywordsCtrl->GetClientSize().GetWidth());
    }
}

void LauncherWindow::onOkButtonPressed([[maybe_unused]] wxCommandEvent& event)
{
    if (m_gameLocationTextbox->GetValue().IsEmpty()) {
        wxMessageBox(ASTr("launcher.missingGameLocation.message", "Please select your game's install location."),
            ASTr("launcher.missingGameLocation.title", "Missing Game Location"), wxOK | wxICON_WARNING, this);
        return;
    }

    if (m_outputLocationTextbox->GetValue().IsEmpty()) {
        wxMessageBox(ASTr("launcher.missingOutputLocation.message", "Please select an output location."),
            ASTr("launcher.missingOutputLocation.title", "Missing Output Location"), wxOK | wxICON_WARNING, this);
        return;
    }

    EndModal(wxID_OK);
}
