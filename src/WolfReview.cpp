/****************************************************************************
 * Wulfy's Tarrasch - Game review (Wolf Eval)
 *  Runs the engine over the current game's main line, shows each player's
 *  accuracy, centipawn loss and a rough Elo estimate, and can mark the
 *  inaccuracies / mistakes / blunders in the game as ?! / ? / ?? with a
 *  comment naming the engine's choice.
 *  Uses the Wolf Eval core in ../wolf-eval/core (same code as wolf-eval.exe).
 ****************************************************************************/
#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE
#endif
#include "wx/wx.h"
#include "wx/progdlg.h"
#include "Appdefs.h"
#include "Objects.h"
#include "Repository.h"
#include "GameLogic.h"
#include "../wolf-eval/core/analysis.h"
#include "../wolf-eval/core/elo.h"
#include "../wolf-eval/core/report.h"
#include <thread>

namespace {

const int kReviewDepth = 18;   // a quick review; wolf-eval.exe goes deeper (--depth 22..40)

class ReviewDialog : public wxDialog
{
public:
    ReviewDialog(wxWindow *parent, const wxString &text)
        : wxDialog(parent, wxID_ANY, "Game review", wxDefaultPosition, wxSize(820, 640),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
        wxTextCtrl *txt = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                         wxTE_MULTILINE | wxTE_READONLY | wxHSCROLL | wxTE_DONTWRAP);
        txt->SetFont(wxFont(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
        top->Add(txt, 1, wxEXPAND | wxALL, 8);
        wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(this, wxID_YES, "Add review to game"), 0, wxRIGHT, 8);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Close"), 0);
        top->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        SetSizer(top);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_YES); }, wxID_YES);
    }
};

const char *nagFor(wolf::Judgement j, char &nag)
{
    switch (j) {
        case wolf::Judgement::Inaccuracy: nag = 6; return "Inaccuracy";
        case wolf::Judgement::Mistake:    nag = 2; return "Mistake";
        case wolf::Judgement::Blunder:    nag = 4; return "Blunder";
        default:                          nag = 0; return nullptr;
    }
}

} // namespace

void GameLogic::CmdGameReview()
{
    if (gd.IsEmpty()) {
        wxMessageBox("There are no moves to review.", "Game review", wxOK | wxICON_INFORMATION, objs.frame);
        return;
    }
    if (state == HUMAN || state == PONDERING || state == THINKING) {
        wxMessageBox("Finish or stop the game against the engine first.", "Game review", wxOK | wxICON_INFORMATION, objs.frame);
        return;
    }

    // The current game as PGN (main line), same text as "Copy game to clipboard"
    gd.FleshOutDate();
    gd.FleshOutMoves();
    std::string head, body;
    gd.ToFileTxtGameDetails(head);
    gd.ToFileTxtGameBody(body);
    std::vector<wolf::PgnGame> games = wolf::parsePgn(head + body);
    if (games.empty() || games[0].sanMoves.empty()) {
        wxMessageBox("Couldn't read the moves of this game.", "Game review", wxOK | wxICON_ERROR, objs.frame);
        return;
    }

    // Tarrasch's own engine and settings
    const EngineConfig &ec = objs.repository->engine;
    std::string enginePath = std::string(ec.m_file.mb_str());
    wolf::UciEngine engine;
    std::string err;
    if (!engine.start(enginePath, err)) {
        wxMessageBox(wxString("Couldn't start the engine:\n") + wxString(err.c_str()) + "\n\nCheck Options > Engine.",
                     "Game review", wxOK | wxICON_ERROR, objs.frame);
        return;
    }
    int hw = (int)std::thread::hardware_concurrency();
    int threads = ec.m_max_cpu_cores > 0 ? ec.m_max_cpu_cores : (hw > 1 ? hw / 2 : 1);
    engine.setOption("Threads", std::to_string(threads));
    engine.setOption("Hash", std::to_string(ec.m_hash > 0 ? ec.m_hash : 256));

    int total = (int)games[0].sanMoves.size() + 1;
    wxProgressDialog progress("Game review", "Analysing with " + wxString(engine.name().c_str()) + "...",
                              total, objs.frame,
                              wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME | wxPD_AUTO_HIDE);
    wolf::AnalysisOptions opt;
    opt.depth = kReviewDepth;
    opt.progress = [&progress, total](int done, int) {
        return progress.Update(done < total ? done : total - 1,
                               wxString::Format("Position %d of %d", done, total));
    };
    wolf::GameReport rep = wolf::analyseGame(games[0], engine, opt);
    engine.stop();
    progress.Hide();
    if (!rep.ok) {
        if (!rep.error.empty())
            wxMessageBox(wxString(rep.error.c_str()), "Game review", wxOK | wxICON_ERROR, objs.frame);
        return;
    }

    ReviewDialog dlg(objs.frame, wxString::FromUTF8(wolf::formatReport(rep).c_str()));
    if (dlg.ShowModal() != wxID_YES)
        return;

    // Mark the main line: NAG + a short comment on every inaccuracy, mistake and blunder
    Atomic begin;
    if (gd.tree.variations.empty())
        return;
    VARIATION &line = gd.tree.variations[0];
    size_t n = line.size() < rep.moves.size() ? line.size() : rep.moves.size();
    for (size_t i = 0; i < n; ++i) {
        const wolf::MoveReport &m = rep.moves[i];
        char nag = 0;
        const char *what = nagFor(m.judgement, nag);
        if (!what)
            continue;
        GAME_MOVE &gm = line[i].game_move;
        if (gm.nag_value1 == 0)
            gm.nag_value1 = nag;
        std::string c = std::string(what) + ". Best was " + m.bestSan + " (" + m.before.str() + ")";
        gm.comment = gm.comment.empty() ? c : gm.comment + " " + c;
    }
    if (n > 0) {
        std::string summary = "Wolf Eval review:" + wolf::formatSummary(rep);
        for (char &ch : summary)
            if (ch == '\n') ch = ' ';
        GAME_MOVE &last = line[n - 1].game_move;
        last.comment = last.comment.empty() ? summary : last.comment + " " + summary;
    }
    gd.Rebuild();
    unsigned long pos = gd.gv.FindEnd();
    atom.Redisplay(pos);
    atom.Undo();
    atom.StatusUpdate();
}
