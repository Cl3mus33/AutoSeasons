#include "GUI/LauncherWindow.hpp"

#include "GUI/SeasonModOverrideDialog.hpp"
#include "GUI/components/PGCustomListctrlChangedEvent.hpp"
#include "common/BethesdaGame.hpp"
#include "util/StringUtil.hpp"

#include <wx/notebook.h>
#include <wx/statline.h>

using namespace std;

namespace {
constexpr int BORDER_SIZE = 5;

// AutoSeasons' own accent colors - deliberately distinct from the plain default system colors the
// rest of the dialog otherwise uses (this is inherited PGPatcher-derived code, and undifferentiated
// wx dialogs from the same codebase end up looking identical). Kept to a small, consistent palette
// rather than styled per-control, so it reads as one deliberate identity, not a random hodgepodge.
const wxColour ACCENT_DARK(27, 94, 32); // header banner background
const wxColour ACCENT(56, 142, 60); // primary button, section label text
const wxColour ACCENT_TEXT(255, 255, 255); // text on top of ACCENT_DARK/ACCENT

// Section labels ("Game Location", "Output Location", etc.) get a consistent bold+accent-colored
// treatment instead of plain default text, so they read as an intentional heading hierarchy rather
// than blending into the help text below them.
auto makeSectionLabel(wxWindow* parent, const wxString& text) -> wxStaticText*
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    wxFont font = label->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    label->SetFont(font);
    label->SetForegroundColour(ACCENT);
    return label;
}
}

LauncherWindow::LauncherWindow(const ASParams& initParams, filesystem::path exePath)
    : wxDialog(nullptr, wxID_ANY, "AutoSeasons", wxDefaultPosition, wxSize(560, 760), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_exePath(std::move(exePath))
    , m_overrideForeignSeasonMods(initParams.overrideForeignSeasonMods)
    , m_overrideForeignSeasonModsByType(initParams.overrideForeignSeasonModsByType)
    , m_foreignSeasonModPriority(initParams.foreignSeasonModPriority)
    , m_foreignSeasonModPriorityByType(initParams.foreignSeasonModPriorityByType)
{
    const wxIcon appIcon(wxICON(IDI_ICON1));
    SetIcon(appIcon);

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    // Header banner: gives the dialog an identity of its own at a glance, instead of opening
    // straight into a plain, undifferentiated settings form indistinguishable from PGPatcher's.
    auto* headerPanel = new wxPanel(this);
    headerPanel->SetBackgroundColour(ACCENT_DARK);
    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* headerIcon = new wxStaticBitmap(headerPanel, wxID_ANY, wxBitmap(appIcon).ConvertToImage().Scale(32, 32, wxIMAGE_QUALITY_HIGH));
    headerSizer->Add(headerIcon, 0, wxALL | wxALIGN_CENTER_VERTICAL, BORDER_SIZE * 2);
    auto* headerTitle = new wxStaticText(headerPanel, wxID_ANY, "AutoSeasons");
    wxFont headerFont = headerTitle->GetFont();
    headerFont.SetPointSize(headerFont.GetPointSize() + 4);
    headerFont.SetWeight(wxFONTWEIGHT_BOLD);
    headerTitle->SetFont(headerFont);
    headerTitle->SetForegroundColour(ACCENT_TEXT);
    headerSizer->Add(headerTitle, 0, wxALIGN_CENTER_VERTICAL);
    headerPanel->SetSizerAndFit(headerSizer);
    mainSizer->Add(headerPanel, 0, wxEXPAND);

    auto* notebook = new wxNotebook(this, wxID_ANY);

    // "General" tab - the actual per-run settings (game/output locations, blocklists).
    auto* generalPanel = new wxPanel(notebook);
    auto* generalSizer = new wxBoxSizer(wxVERTICAL);

    auto* introText = new wxStaticText(generalPanel, wxID_ANY,
        ASTr("launcher.intro",
            "Scans your load order for seasonal texture variants (e.g. rock01_AUT.dds next to "
            "rock01.dds) and generates the plugin + Data/Seasons/*.ini files for the Seasons of "
            "Skyrim SKSE plugin."));
    introText->Wrap(490);
    generalSizer->Add(introText, 0, wxALL, BORDER_SIZE * 2);

    // Config profile - a single install (e.g. shared outside any one modlist) can still keep
    // distinct settings per use case by saving/loading separate JSON files here, instead of
    // relying on AutoSeasons_config.json next to the exe (which only isolates settings when each
    // modlist gets its own copy of the exe). Mirrors PGPatcher's own Load/Save Config pattern.
    auto* configProfileLabel = makeSectionLabel(generalPanel, ASTr("launcher.configProfile.label", "Config Profile"));
    generalSizer->Add(configProfileLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    auto* loadConfigButton = new wxButton(generalPanel, wxID_ANY, ASTr("launcher.configProfile.load", "Load Config..."));
    loadConfigButton->Bind(wxEVT_BUTTON, &LauncherWindow::onLoadConfig, this);
    auto* saveConfigButton
        = new wxButton(generalPanel, wxID_ANY, ASTr("launcher.configProfile.saveAs", "Save Config As..."));
    saveConfigButton->Bind(wxEVT_BUTTON, &LauncherWindow::onSaveConfigAs, this);

    auto* configProfileSizer = new wxBoxSizer(wxHORIZONTAL);
    configProfileSizer->Add(loadConfigButton, 0, wxALL, BORDER_SIZE);
    configProfileSizer->Add(saveConfigButton, 0, wxALL, BORDER_SIZE);
    generalSizer->Add(configProfileSizer, 0);

    // Game location
    auto* gameLocationLabel = makeSectionLabel(generalPanel, ASTr("launcher.gameLocation.label", "Game Location"));
    generalSizer->Add(gameLocationLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    // Running the exe directly (double-clicked from Explorer) only sees the game's real Data
    // folder, not a mod manager's merged/virtual view of it - the single most common way a first
    // run silently produces an incomplete result (it "succeeds", just against the wrong files).
    auto* gameLocationWarning = new wxStaticText(generalPanel, wxID_ANY,
        ASTr("launcher.gameLocation.mo2Warning",
            "If you use Mod Organizer 2 or Vortex, run AutoSeasons through its tool list/dashboard, "
            "not by double-clicking the exe in Explorer - otherwise it only sees your real Data "
            "folder, not your installed mods, and silently produces an incomplete result."));
    gameLocationWarning->SetForegroundColour(wxColour(180, 95, 0)); // amber - distinct from the accent green, reads as caution
    gameLocationWarning->Wrap(490);
    generalSizer->Add(gameLocationWarning, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_gameLocationTextbox = new wxTextCtrl(generalPanel, wxID_ANY, initParams.gameDir.wstring());
    auto* gameBrowseButton = new wxButton(generalPanel, wxID_ANY, ASTr("common.browse", "Browse"));
    gameBrowseButton->Bind(wxEVT_BUTTON, &LauncherWindow::onBrowseGameLocation, this);

    auto* gameLocationSizer = new wxBoxSizer(wxHORIZONTAL);
    gameLocationSizer->Add(m_gameLocationTextbox, 1, wxEXPAND | wxALL, BORDER_SIZE);
    gameLocationSizer->Add(gameBrowseButton, 0, wxALL, BORDER_SIZE);
    generalSizer->Add(gameLocationSizer, 0, wxEXPAND);

    // Game type
    auto* gameTypeLabel = makeSectionLabel(generalPanel, ASTr("launcher.gameType.label", "Game Type"));
    generalSizer->Add(gameTypeLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

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
    m_gameTypeChoice = new wxChoice(generalPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, gameTypeChoices);
    m_gameTypeChoice->SetSelection(selectedGameTypeIndex);
    generalSizer->Add(m_gameTypeChoice, 0, wxEXPAND | wxALL, BORDER_SIZE);

    // Output location
    auto* outputLocationLabel = makeSectionLabel(generalPanel, ASTr("launcher.outputLocation.label", "Output Location"));
    generalSizer->Add(outputLocationLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_outputLocationTextbox = new wxTextCtrl(generalPanel, wxID_ANY, initParams.outputDir.wstring());
    auto* outputBrowseButton = new wxButton(generalPanel, wxID_ANY, ASTr("common.browse", "Browse"));
    outputBrowseButton->Bind(wxEVT_BUTTON, &LauncherWindow::onBrowseOutputLocation, this);

    auto* outputLocationSizer = new wxBoxSizer(wxHORIZONTAL);
    outputLocationSizer->Add(m_outputLocationTextbox, 1, wxEXPAND | wxALL, BORDER_SIZE);
    outputLocationSizer->Add(outputBrowseButton, 0, wxALL, BORDER_SIZE);
    generalSizer->Add(outputLocationSizer, 0, wxEXPAND);

    // Mesh blocklist - an inline editable table (not a separate popup), so the current rules are
    // always visible at a glance. Right click, or double-click a row, to add/remove entries.
    auto* blocklistLabel = makeSectionLabel(generalPanel, ASTr("launcher.blocklist.label", "Seasonal Variation Blocklist"));
    generalSizer->Add(blocklistLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    auto* blocklistHelpText = new wxStaticText(generalPanel, wxID_ANY,
        ASTr("launcher.blocklist.help",
            "Meshes matching a rule here are skipped by seasonal variation (useful for interior "
            "meshes, since Seasons of Skyrim only swaps records in exterior worldspaces). Wildcards "
            "(*) allowed, e.g. \"*/interiors/*\". Right click to add/remove rows."));
    blocklistHelpText->Wrap(520);
    generalSizer->Add(blocklistHelpText, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_blocklistCtrl = new PGModifiableListCtrl(
        generalPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_EDIT_LABELS | wxLC_NO_HEADER);
    m_blocklistCtrl->AppendColumn(ASTr("launcher.blocklist.column", "Rule"), wxLIST_FORMAT_LEFT, wxLIST_AUTOSIZE_USEHEADER);
    m_blocklistCtrl->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);

    long blocklistIndex = 0;
    for (const auto& rule : initParams.meshBlockList) {
        m_blocklistCtrl->InsertItem(blocklistIndex++, wxString(rule));
    }
    m_blocklistCtrl->InsertItem(m_blocklistCtrl->GetItemCount(), "");

    generalSizer->Add(m_blocklistCtrl, 1, wxEXPAND | wxALL, BORDER_SIZE);

    // Season-locked EditorID keywords - same inline editable table pattern as the mesh blocklist
    // above. Records whose EditorID contains one of these (case-insensitive substring match) are
    // skipped by seasonal variation entirely, e.g. so a mod's own "...Coast01"/"...River01"
    // statics never get a pointless seasonal duplicate regardless of which texture files exist.
    auto* editorIDKeywordsLabel
        = makeSectionLabel(generalPanel, ASTr("launcher.editorIdKeywords.label", "Season-Locked EditorID Keywords"));
    generalSizer->Add(editorIDKeywordsLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    auto* editorIDKeywordsHelpText = new wxStaticText(generalPanel, wxID_ANY,
        ASTr("launcher.editorIdKeywords.help",
            "Records whose EditorID contains one of these words (case-insensitive) are skipped by "
            "seasonal variation entirely - useful for names that already signal a fixed "
            "season/biome, e.g. \"Coast\" or \"River\". Right click to add/remove rows."));
    editorIDKeywordsHelpText->Wrap(520);
    generalSizer->Add(editorIDKeywordsHelpText, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_editorIDKeywordsCtrl = new PGModifiableListCtrl(
        generalPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_EDIT_LABELS | wxLC_NO_HEADER);
    m_editorIDKeywordsCtrl->AppendColumn(
        ASTr("launcher.editorIdKeywords.column", "Keyword"), wxLIST_FORMAT_LEFT, wxLIST_AUTOSIZE_USEHEADER);
    m_editorIDKeywordsCtrl->SetColumnWidth(0, wxLIST_AUTOSIZE_USEHEADER);

    long editorIDKeywordIndex = 0;
    for (const auto& keyword : initParams.seasonLockedEditorIDKeywords) {
        m_editorIDKeywordsCtrl->InsertItem(editorIDKeywordIndex++, wxString(keyword));
    }
    m_editorIDKeywordsCtrl->InsertItem(m_editorIDKeywordsCtrl->GetItemCount(), "");

    generalSizer->Add(m_editorIDKeywordsCtrl, 1, wxEXPAND | wxALL, BORDER_SIZE);

    // Foreign season mod overrides - opens a separate dialog rather than an inline list, since
    // populating it requires scanning the (possibly not-yet-saved) game location above, not just
    // editing plain text like the two lists above it.
    auto* seasonModOverridesButton = new wxButton(
        generalPanel, wxID_ANY, ASTr("launcher.seasonModOverrides.button", "Manage Season Mod Conflicts..."));
    seasonModOverridesButton->Bind(wxEVT_BUTTON, &LauncherWindow::onManageSeasonModOverrides, this);
    generalSizer->Add(seasonModOverridesButton, 0, wxALL, BORDER_SIZE);

    // Remove grass in winter - on by default (matches ASParams::removeGrassInWinter's own default),
    // dropping any grass slot with no detected winter mesh variant rather than leaving it visible
    // year-round under snow. Persisted like any other General-tab setting (unlike dry run below).
    m_removeGrassInWinterCheckbox = new wxCheckBox(generalPanel, wxID_ANY,
        ASTr("launcher.removeGrassInWinter.label", "Remove grass in winter if no winter variant exists"));
    m_removeGrassInWinterCheckbox->SetValue(initParams.removeGrassInWinter);
    generalSizer->Add(m_removeGrassInWinterCheckbox, 0, wxALL, BORDER_SIZE);

    // Dry run - deliberately not persisted to config (ASParams::dryRun is never read from/written
    // to AutoSeasons_config.json), so it always starts unchecked and has to be opted into for each
    // run rather than silently staying on and confusing a future "why didn't anything change" run.
    m_dryRunCheckbox = new wxCheckBox(generalPanel, wxID_ANY,
        ASTr("launcher.dryRun.label", "Preview only (dry run) - scan and log what would happen, write nothing"));
    generalSizer->Add(m_dryRunCheckbox, 0, wxALL, BORDER_SIZE);

    // ESM flag: always on (matches PGPatcher's own output plugin convention). ESL-flagging
    // happens automatically and safely based on record count, independent of this - not worth
    // exposing as a separate choice, it was just a confusing extra decision.

    generalPanel->SetSizer(generalSizer);
    notebook->AddPage(generalPanel, ASTr("launcher.tab.general", "General"));

    // "Options" tab - app-wide preferences (language, theme) rather than per-run settings.
    auto* optionsPanel = new wxPanel(notebook);
    auto* optionsSizer = new wxBoxSizer(wxVERTICAL);

    // Language selector - changing it immediately relaunches the window (see onLanguageChanged)
    // rather than requiring a separate settings dialog/OK click, since AutoSeasons' launcher is
    // small enough that a full rebuild is cheap and this keeps the UX to a single click.
    auto* languageLabel = makeSectionLabel(optionsPanel, ASTr("launcher.language.label", "Language"));
    optionsSizer->Add(languageLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    m_languages = ASLocale::getAvailableLanguages();
    wxArrayString languageChoices;
    int selectedLanguageIndex = 0;
    for (size_t i = 0; i < m_languages.size(); i++) {
        languageChoices.Add(m_languages.at(i).displayName);
        if (m_languages.at(i).code == ASLocale::getCurrentLanguage()) {
            selectedLanguageIndex = static_cast<int>(i);
        }
    }
    m_languageChoice = new wxChoice(optionsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, languageChoices);
    if (!m_languages.empty()) {
        m_languageChoice->SetSelection(selectedLanguageIndex);
    }
    m_languageChoice->Bind(wxEVT_CHOICE, &LauncherWindow::onLanguageChanged, this);
    optionsSizer->Add(m_languageChoice, 0, wxEXPAND | wxALL, BORDER_SIZE);

    // Theme selector - same immediate-relaunch pattern as the language selector, since applying a
    // wxWidgets appearance change requires it to be set before the window it affects is created.
    auto* themeLabel = makeSectionLabel(optionsPanel, ASTr("launcher.theme.label", "Theme"));
    optionsSizer->Add(themeLabel, 0, wxLEFT | wxRIGHT | wxTOP, BORDER_SIZE);

    wxArrayString themeChoices;
    themeChoices.Add(ASTr("launcher.theme.system", "System"));
    themeChoices.Add(ASTr("launcher.theme.light", "Light"));
    themeChoices.Add(ASTr("launcher.theme.dark", "Dark"));
    m_themeChoice = new wxChoice(optionsPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, themeChoices);
    int selectedThemeIndex = 0;
    if (initParams.uiTheme == "light") {
        selectedThemeIndex = 1;
    } else if (initParams.uiTheme == "dark") {
        selectedThemeIndex = 2;
    }
    m_themeChoice->SetSelection(selectedThemeIndex);
    m_themeChoice->Bind(wxEVT_CHOICE, &LauncherWindow::onThemeChanged, this);
    optionsSizer->Add(m_themeChoice, 0, wxEXPAND | wxALL, BORDER_SIZE);

    optionsPanel->SetSizer(optionsSizer);
    notebook->AddPage(optionsPanel, ASTr("launcher.tab.options", "Options"));

    mainSizer->Add(notebook, 1, wxEXPAND | wxALL, BORDER_SIZE);

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

    mainSizer->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxALL, BORDER_SIZE);

    // Buttons
    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* cancelButton = new wxButton(this, wxID_CANCEL, ASTr("common.cancel", "Cancel"));
    m_okButton = new wxButton(this, wxID_ANY, ASTr("launcher.startButton", "Start Patching"));
    m_okButton->SetBackgroundColour(ACCENT);
    m_okButton->SetForegroundColour(ACCENT_TEXT);
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
    switch (m_themeChoice->GetSelection()) {
    case 1:
        outParams.uiTheme = "light";
        break;
    case 2:
        outParams.uiTheme = "dark";
        break;
    default:
        outParams.uiTheme = "system";
        break;
    }
    outParams.removeGrassInWinter = m_removeGrassInWinterCheckbox->GetValue();
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

    outParams.overrideForeignSeasonMods = m_overrideForeignSeasonMods;
    outParams.overrideForeignSeasonModsByType = m_overrideForeignSeasonModsByType;
    outParams.foreignSeasonModPriority = m_foreignSeasonModPriority;
    outParams.foreignSeasonModPriorityByType = m_foreignSeasonModPriorityByType;
    outParams.dryRun = m_dryRunCheckbox->GetValue();
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

    ASLocale::init(m_exePath / "AutoSeasons_translations", selectedLang.code);
    EndModal(RESULT_RELAUNCH);
}

void LauncherWindow::onThemeChanged([[maybe_unused]] wxCommandEvent& event)
{
    // Unlike a language change, this needs a full process restart, not just an internal rebuild -
    // wx's MSW dark mode support turned out to be a one-way switch once enabled for a process, so
    // an in-process relaunch can't reliably get back to a lighter appearance after a darker one.
    // See main.cpp's handling of RESULT_RESTART.
    EndModal(RESULT_RESTART);
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

void LauncherWindow::onManageSeasonModOverrides([[maybe_unused]] wxCommandEvent& event)
{
    // Reads the live textbox/choice values (not initParams) so a game location just browsed to in
    // this same session is what gets scanned, rather than whatever was loaded from config at
    // window-open time.
    SeasonModOverrideDialog dialog(this, m_gameLocationTextbox->GetValue().ToStdWstring(),
        BethesdaGame::getGameTypes()[static_cast<size_t>(m_gameTypeChoice->GetSelection())], m_overrideForeignSeasonMods,
        m_overrideForeignSeasonModsByType, m_foreignSeasonModPriority, m_foreignSeasonModPriorityByType);
    if (dialog.ShowModal() == wxID_OK) {
        m_overrideForeignSeasonMods = dialog.getSelectedOverrides();
        m_overrideForeignSeasonModsByType = dialog.getOverridesByType();
        m_foreignSeasonModPriority = dialog.getPriorityOrder();
        m_foreignSeasonModPriorityByType = dialog.getPriorityByType();
    }
}

void LauncherWindow::onLoadConfig([[maybe_unused]] wxCommandEvent& event)
{
    wxFileDialog dialog(this, ASTr("launcher.configProfile.loadDialogTitle", "Load Config"), wxEmptyString, wxEmptyString,
        ASTr("launcher.configProfile.fileFilter", "JSON files (*.json)|*.json|All files (*.*)|*.*"),
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    applyLoadedParams(ASConfig::loadFrom(filesystem::path(dialog.GetPath().ToStdWstring())));
}

void LauncherWindow::onSaveConfigAs([[maybe_unused]] wxCommandEvent& event)
{
    wxFileDialog dialog(this, ASTr("launcher.configProfile.saveDialogTitle", "Save Config As"), wxEmptyString,
        "AutoSeasons_config.json", ASTr("launcher.configProfile.fileFilter", "JSON files (*.json)|*.json|All files (*.*)|*.*"),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    ASParams current;
    getParams(current);
    ASConfig::saveTo(filesystem::path(dialog.GetPath().ToStdWstring()), current);
}

void LauncherWindow::applyLoadedParams(const ASParams& params)
{
    m_gameLocationTextbox->SetValue(params.gameDir.wstring());
    m_outputLocationTextbox->SetValue(params.outputDir.wstring());

    const auto& gameTypes = BethesdaGame::getGameTypes();
    for (size_t i = 0; i < gameTypes.size(); i++) {
        if (gameTypes[i] == params.gameType) {
            m_gameTypeChoice->SetSelection(static_cast<int>(i));
            break;
        }
    }

    m_blocklistCtrl->DeleteAllItems();
    long blocklistIndex = 0;
    for (const auto& rule : params.meshBlockList) {
        m_blocklistCtrl->InsertItem(blocklistIndex++, wxString(rule));
    }
    m_blocklistCtrl->InsertItem(m_blocklistCtrl->GetItemCount(), "");

    m_editorIDKeywordsCtrl->DeleteAllItems();
    long editorIDKeywordIndex = 0;
    for (const auto& keyword : params.seasonLockedEditorIDKeywords) {
        m_editorIDKeywordsCtrl->InsertItem(editorIDKeywordIndex++, wxString(keyword));
    }
    m_editorIDKeywordsCtrl->InsertItem(m_editorIDKeywordsCtrl->GetItemCount(), "");

    m_overrideForeignSeasonMods = params.overrideForeignSeasonMods;
    m_overrideForeignSeasonModsByType = params.overrideForeignSeasonModsByType;
    m_foreignSeasonModPriority = params.foreignSeasonModPriority;
    m_foreignSeasonModPriorityByType = params.foreignSeasonModPriorityByType;

    updateListColumnWidths();
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
