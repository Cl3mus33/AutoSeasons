#pragma once

#include "ASConfig.hpp"
#include "ASLocale.hpp"
#include "GUI/components/PGModifiableListCtrl.hpp"

#include <wx/wx.h>

#include <filesystem>
#include <vector>

/**
 * @brief Main settings dialog for AutoSeasons: game location, output location, game type, and
 * mesh blacklist. Shown modally; on OK, getParams() returns the edited settings.
 */
class LauncherWindow : public wxDialog {
public:
    /** ShowModal result indicating the launcher should be rebuilt (e.g. after a language change) */
    constexpr static int RESULT_RELAUNCH = wxID_HIGHEST + 1;
    /** ShowModal result indicating the whole process should be restarted (after a theme change -
     * wx's MSW dark mode support can't be reliably re-toggled within one already-running process) */
    constexpr static int RESULT_RESTART = wxID_HIGHEST + 2;

    LauncherWindow(const ASParams& initParams, std::filesystem::path exePath);

    void getParams(ASParams& outParams) const;

private:
    std::filesystem::path m_exePath;
    std::vector<ASLocale::Language> m_languages;

    wxChoice* m_languageChoice;
    wxChoice* m_themeChoice;
    wxTextCtrl* m_gameLocationTextbox;
    wxTextCtrl* m_outputLocationTextbox;
    wxChoice* m_gameTypeChoice;
    PGModifiableListCtrl* m_blocklistCtrl;
    PGModifiableListCtrl* m_editorIDKeywordsCtrl;
    wxButton* m_okButton;

    void onLanguageChanged(wxCommandEvent& event);
    void onThemeChanged(wxCommandEvent& event);
    void onBrowseGameLocation(wxCommandEvent& event);
    void onBrowseOutputLocation(wxCommandEvent& event);
    void onOkButtonPressed(wxCommandEvent& event);
    void updateListColumnWidths();
};
