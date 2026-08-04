#pragma once

#include "ASConfig.hpp"

#include <wx/collpane.h>
#include <wx/gauge.h>
#include <wx/wx.h>

#include <filesystem>
#include <thread>

/**
 * @brief Modal dialog that runs the AutoSeasons pipeline on a background thread. Shows a small,
 * compact status view by default (an indeterminate progress bar plus the latest log line); the
 * full scrolling log stays available behind a collapsible "Show details" pane for troubleshooting.
 * The Close button stays disabled until the run finishes (success or failure), so the window
 * never disappears out from under the user mid-run.
 */
class ProgressWindow : public wxDialog {
public:
    ProgressWindow(const ASParams& params, const std::filesystem::path& exePath);
    ~ProgressWindow() override;

    ProgressWindow(const ProgressWindow&) = delete;
    auto operator=(const ProgressWindow&) -> ProgressWindow& = delete;
    ProgressWindow(ProgressWindow&&) = delete;
    auto operator=(ProgressWindow&&) -> ProgressWindow& = delete;

    // Called from the worker thread via a thread-safe sink; queues the actual UI update on the
    // main thread with wxTheApp->CallAfter.
    void appendLog(const wxString& text);

private:
    wxStaticText* m_statusText;
    wxGauge* m_progressGauge;
    wxCollapsiblePane* m_detailsPane;
    wxTextCtrl* m_logCtrl;
    wxButton* m_closeButton;
    wxTimer m_pulseTimer;
    std::thread m_workerThread;

    void onWorkerFinished(bool success);
    void onCloseButtonPressed(wxCommandEvent& event);
    void onDetailsPaneChanged(wxCollapsiblePaneEvent& event);
};
