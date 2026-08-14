#include "GUI/ProgressWindow.hpp"

#include "ASLocale.hpp"
#include "AutoSeasonsRunner.hpp"

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <exception>
#include <mutex>
#include <regex>

using namespace std;

namespace {
constexpr int BORDER_SIZE = 5;
constexpr int GAUGE_WIDTH = 320;
constexpr int GAUGE_HEIGHT = 18;
constexpr int PULSE_INTERVAL_MS = 100;
constexpr int WRAP_WIDTH = 340;

// Matches LauncherWindow's own accent colors, so the two windows read as one identity.
const wxColour ACCENT(56, 142, 60);
const wxColour ACCENT_TEXT(255, 255, 255);

// Strips spdlog's "[timestamp] [level] " prefix (two bracketed groups), leaving just the
// human-readable message - the raw prefixed line is too noisy for the compact status label
// (it still shows up in full in the "Show details" log panel).
auto stripLogPrefix(const wxString& raw) -> wxString
{
    size_t pos = 0;
    int bracketsClosed = 0;
    while (bracketsClosed < 2) {
        const auto closeBracket = raw.find(']', pos);
        if (closeBracket == wxString::npos) {
            break;
        }
        pos = closeBracket + 1;
        bracketsClosed++;
    }

    wxString result = bracketsClosed == 2 ? raw.Mid(pos) : raw;
    result.Trim(true).Trim(false);
    return result;
}

// Forwards every log line to the ProgressWindow's log panel, hopping onto the main/GUI thread
// via wxTheApp->CallAfter since this sink is fed from the worker thread.
class WxLogSink : public spdlog::sinks::base_sink<mutex> {
public:
    explicit WxLogSink(ProgressWindow* window)
        : m_window(window)
    {
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override
    {
        spdlog::memory_buf_t formatted;
        base_sink<mutex>::formatter_->format(msg, formatted);
        auto text = wxString::FromUTF8(fmt::to_string(formatted));

        auto* window = m_window;
        wxTheApp->CallAfter([window, text]() -> void { window->appendLog(text); });
    }

    void flush_() override { }

private:
    ProgressWindow* m_window;
};
}

ProgressWindow::ProgressWindow(const ASParams& params, const filesystem::path& exePath)
    : wxDialog(nullptr, wxID_ANY, "AutoSeasons", wxDefaultPosition, wxSize(420, 170), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_pulseTimer(this)
    , m_wasDryRun(params.dryRun)
{
    const wxIcon appIcon(wxICON(IDI_ICON1));
    SetIcon(appIcon);

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);

    m_statusText = new wxStaticText(this, wxID_ANY, ASTr("progress.starting", "Starting..."));
    m_statusText->Wrap(WRAP_WIDTH);
    mainSizer->Add(m_statusText, 0, wxEXPAND | wxALL, BORDER_SIZE * 2);

    m_progressGauge = new wxGauge(
        this, wxID_ANY, 100, wxDefaultPosition, wxSize(GAUGE_WIDTH, GAUGE_HEIGHT), wxGA_SMOOTH);
    mainSizer->Add(m_progressGauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);

    m_detailsPane = new wxCollapsiblePane(this, wxID_ANY, ASTr("progress.showDetails", "Show details"));
    auto* paneWindow = m_detailsPane->GetPane();
    auto* paneSizer = new wxBoxSizer(wxVERTICAL);

    m_logCtrl = new wxTextCtrl(paneWindow, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 260),
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    wxFont fixedFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    m_logCtrl->SetFont(fixedFont);
    // Explicit high-contrast colors: some Windows dark-mode themes give this control a dark
    // background but leave the default text color dark too, making the log nearly unreadable.
    m_logCtrl->SetBackgroundColour(wxColour(24, 24, 24));
    m_logCtrl->SetForegroundColour(wxColour(220, 220, 220));
    m_logCtrl->SetDefaultStyle(wxTextAttr(wxColour(220, 220, 220), wxColour(24, 24, 24)));
    paneSizer->Add(m_logCtrl, 1, wxEXPAND);
    paneWindow->SetSizer(paneSizer);

    mainSizer->Add(m_detailsPane, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, BORDER_SIZE * 2);
    m_detailsPane->Bind(wxEVT_COLLAPSIBLEPANE_CHANGED, &ProgressWindow::onDetailsPaneChanged, this);

    m_closeButton = new wxButton(this, wxID_ANY, ASTr("progress.working", "Working..."));
    m_closeButton->SetBackgroundColour(ACCENT);
    m_closeButton->SetForegroundColour(ACCENT_TEXT);
    m_closeButton->Enable(false);
    m_closeButton->Bind(wxEVT_BUTTON, &ProgressWindow::onCloseButtonPressed, this);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(m_closeButton, 0, wxALL, BORDER_SIZE);
    mainSizer->Add(buttonSizer, 0, wxEXPAND);

    SetSizerAndFit(mainSizer);
    Centre();

    // Closing the window (X button) while the worker is still running would tear down this
    // object out from under the background thread - refuse it until the run has finished.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) -> void {
        if (!m_closeButton->IsEnabled()) {
            event.Veto();
            return;
        }
        EndModal(wxID_OK);
    });

    Bind(wxEVT_TIMER, [this](wxTimerEvent&) -> void { m_progressGauge->Pulse(); });
    m_pulseTimer.Start(PULSE_INTERVAL_MS);

    m_workerThread = thread([this, params, exePath]() -> void {
        bool success = true;
        try {
            ASParams localParams = params;
            const vector<spdlog::sink_ptr> sinks { make_shared<WxLogSink>(this) };
            m_runSummary = AutoSeasonsRunner::run(localParams, exePath, sinks);
        } catch (const exception& e) {
            success = false;
            const string message = e.what();
            wxTheApp->CallAfter(
                [this, message]() -> void { appendLog("\n[ERROR] " + wxString::FromUTF8(message) + "\n"); });
        } catch (...) {
            // Anything not deriving from std::exception (e.g. a raw type thrown from a dependency)
            // would otherwise escape this thread function uncaught, which calls std::terminate()
            // and kills the whole app with no message at all - the console is hidden in GUI mode,
            // so that would look exactly like an unexplained crash rather than a reported error.
            success = false;
            wxTheApp->CallAfter([this]() -> void {
                appendLog("\n[ERROR] An unexpected error occurred - see AutoSeasons.log for details.\n");
            });
        }

        wxTheApp->CallAfter([this, success]() -> void { onWorkerFinished(success); });
    });
}

ProgressWindow::~ProgressWindow()
{
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void ProgressWindow::appendLog(const wxString& text)
{
    m_logCtrl->AppendText(text);

    // Show the latest non-empty line as the compact status text, with the log prefix stripped.
    wxString trimmed = text;
    trimmed.Trim(true).Trim(false);
    if (trimmed.IsEmpty()) {
        return;
    }

    const auto lastNewline = trimmed.find_last_of('\n');
    if (lastNewline != wxString::npos) {
        trimmed = trimmed.Mid(lastNewline + 1);
        trimmed.Trim(true).Trim(false);
    }

    const wxString message = stripLogPrefix(trimmed);
    if (message.IsEmpty()) {
        return;
    }

    m_statusText->SetLabel(message);
    m_statusText->Wrap(WRAP_WIDTH);
    // Wrap() can change how many lines the label needs (e.g. a longer translated status string
    // wrapping to 2 lines instead of 1) - without re-running the sizer, the gauge below keeps its
    // old position and visually overlaps the now-taller text instead of being pushed down.
    Layout();
    GetSizer()->Fit(this);

    // Some log lines already carry a "[N%]" progress figure (e.g. "Loading NIFs Progress:
    // 9925/17722 [56%]") - when present, drive the gauge with it instead of just pulsing.
    static const wregex percentPattern(LR"(\[(\d{1,3})%\])");
    wsmatch match;
    const wstring stdMessage = message.ToStdWstring();
    if (regex_search(stdMessage, match, percentPattern)) {
        const int pct = clamp(stoi(match[1].str()), 0, 100);
        m_pulseTimer.Stop();
        m_progressGauge->SetValue(pct);
    } else if (!m_pulseTimer.IsRunning()) {
        m_pulseTimer.Start(PULSE_INTERVAL_MS);
    }
}

void ProgressWindow::onWorkerFinished(bool success)
{
    m_pulseTimer.Stop();
    m_progressGauge->SetValue(100);
    m_statusText->SetLabel(success ? ASTr("progress.done", "Done.") : ASTr("progress.failed", "Failed - see details below."));
    m_statusText->Wrap(WRAP_WIDTH);
    m_closeButton->SetLabel(success ? ASTr("progress.doneClose", "Done - Close") : ASTr("progress.failedClose", "Failed - Close"));
    m_closeButton->Enable(true);

    if (!success && !m_detailsPane->IsExpanded()) {
        m_detailsPane->Expand();
        Layout();
        GetSizer()->Fit(this);
    }

    if (success) {
        // A generic "Generation complete" reads identically whether 1,800 records were created or
        // zero (e.g. the game/output directory was wrong, or the blocklist/keywords are too
        // aggressive) - showing the real breakdown lets a user immediately tell a genuine "this
        // load order has no seasonal content" apart from a misconfiguration, instead of assuming
        // the run succeeded and only noticing nothing changed once back in-game.
        wxString message;
        if (m_runSummary.totalRecords == 0) {
            message = m_wasDryRun
                ? ASTr("progress.completion.zeroDryRunMessage",
                      "Preview complete, but 0 seasonal records would be created. Double-check your game "
                      "directory and blocklist/keyword settings - this usually means AutoSeasons didn't find "
                      "the load order it expected.")
                : ASTr("progress.completion.zeroMessage",
                      "Generation complete, but 0 seasonal records were created. Double-check your game "
                      "directory and blocklist/keyword settings - this usually means AutoSeasons didn't "
                      "find the load order it expected.");
        } else {
            wxString breakdown;
            for (const auto& [recordType, count] : m_runSummary.countsByType) {
                if (!breakdown.IsEmpty()) {
                    breakdown += ", ";
                }
                breakdown += wxString::Format("%s: %zu", wxString(recordType), count);
            }
            message = m_wasDryRun
                ? wxString::Format(
                      ASTr("progress.completion.dryRunMessage",
                          "Preview complete - would create %zu seasonal record(s) (%s). Nothing was written to "
                          "disk."),
                      m_runSummary.totalRecords, breakdown)
                : wxString::Format(
                      ASTr("progress.completion.message",
                          "Generation complete - created %zu seasonal record(s) (%s). Don't forget to enable "
                          "AutoSeasons.esp in your mod manager's plugin list if it isn't already active."),
                      m_runSummary.totalRecords, breakdown);

            if (m_runSummary.unresolvedForeignConflicts > 0) {
                message += wxString::Format(
                    ASTr("progress.completion.conflictsSuffix",
                        "\n\n%zu record/season combination(s) are covered by multiple foreign mods with no "
                        "priority set between them - see the log, or open \"Manage Season Mod Conflicts\" in "
                        "the launcher to resolve which one should win."),
                    m_runSummary.unresolvedForeignConflicts);
            }
        }

        wxMessageBox(message, ASTr("progress.completion.title", "AutoSeasons"), wxOK | wxICON_INFORMATION, this);
        // Dismissing this message box was previously a dead end - the dialog stayed open behind
        // it, requiring a second click on "Done - Close" for no reason. OK here means the same
        // thing as that button, so just close.
        EndModal(wxID_OK);
    }
}

void ProgressWindow::onCloseButtonPressed([[maybe_unused]] wxCommandEvent& event) { EndModal(wxID_OK); }

void ProgressWindow::onDetailsPaneChanged([[maybe_unused]] wxCollapsiblePaneEvent& event)
{
    Layout();
    GetSizer()->Fit(this);
}
