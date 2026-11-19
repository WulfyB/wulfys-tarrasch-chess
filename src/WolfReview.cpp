/****************************************************************************
 * Wulfy's Tarrasch - Game review (Wolf Eval)
 *  Commands > Game review (accuracy)...
 *   1. A small settings window: how deep to analyse, and which method.
 *   2. Runs the engine over the current game's main line (cancellable).
 *   3. Opens the review window (it stays open while you use the board):
 *      - a card per player: accuracy, rough Elo, ACPL, best moves and
 *        inaccuracy / mistake / blunder counts
 *      - an evaluation graph; click or drag on it to step through the game
 *      - a coloured move list; click a move to show it on the board,
 *        "Previous / Next mistake" to hop between errors
 *      - Copy report (the full text review), Add review to game
 *        (?! / ? / ?? plus "Best was ..." comments and a summary)
 *   Settings live in WolfReview.ini, and every review is added to
 *   "WolfReview history.csv" (both in Tarrasch's documents folder). Each
 *   player card compares with earlier games reviewed the same way only
 *   (same depth, method and engine).
 *  Uses the Wolf Eval core in ../wolf-eval/core (same code as wolf-eval.exe).
 ****************************************************************************/
#ifndef _CRT_SECURE_NO_DEPRECATE
#define _CRT_SECURE_NO_DEPRECATE
#endif
#include "wx/wx.h"
#include "wx/progdlg.h"
#include "wx/listctrl.h"
#include "wx/dcbuffer.h"
#include "wx/clipbrd.h"
#include "Appdefs.h"
#include "Objects.h"
#include "Repository.h"
#include "GameLogic.h"
#include "../wolf-eval/core/analysis.h"
#include "../wolf-eval/core/elo.h"
#include "../wolf-eval/core/report.h"
#include "../wolf-eval/core/history.h"
#include "wx/utils.h"
#include "wx/filename.h"
#include <functional>
#include <thread>

namespace {

// ---------------------------------------------------------------- settings

const int kDepths[] = { 14, 18, 22, 26 };

std::string settingsPath()
{
    wxString dir = objs.repository->nv.m_doc_dir;
    if (dir.empty())
        dir = wxFileName::GetCwd();
    return std::string((dir + "/WolfReview.ini").mb_str());
}

class ReviewSettingsDialog : public wxDialog
{
public:
    wxRadioBox *depth, *method;
    wxCheckBox *record;
    int customDepth = 0;     // a depth set by hand in the .ini that isn't one of the four

    ReviewSettingsDialog(wxWindow *parent, const wolf::ReviewSettings &cur)
        : wxDialog(parent, wxID_ANY, "Game review")
    {
        wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);
        wxArrayString depths;
        depths.Add("Quick (depth 14)"); depths.Add("Standard (depth 18)");
        depths.Add("Deep (depth 22)");  depths.Add("Very deep (depth 26)");
        int sel = -1;
        for (int i = 0; i < 4; ++i)
            if (kDepths[i] == cur.depth) sel = i;
        if (sel < 0) {
            customDepth = cur.depth;
            depths.Add(wxString::Format("From WolfReview.ini (depth %d)", cur.depth));
            sel = 4;
        }
        depth = new wxRadioBox(this, wxID_ANY, "How hard should the engine look?", wxDefaultPosition,
                               wxDefaultSize, depths, 1, wxRA_SPECIFY_COLS);
        depth->SetSelection(sel);
        top->Add(depth, 0, wxEXPAND | wxALL, 10);
        wxString methods[] = { "Best move vs. the move played (recommended)",
                               "Lichess style (evaluation before vs. after each move)" };
        method = new wxRadioBox(this, wxID_ANY, "How to judge each move", wxDefaultPosition,
                                wxDefaultSize, 2, methods, 1, wxRA_SPECIFY_COLS);
        method->SetSelection(cur.method == wolf::Method::Lichess ? 1 : 0);
        top->Add(method, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);
        record = new wxCheckBox(this, wxID_ANY, "Save the results to my review history (CSV)");
        record->SetValue(cur.recordHistory);
        top->Add(record, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        wxStaticText *note = new wxStaticText(this, wxID_ANY,
            "Deeper is more accurate but slower. Uses the engine from Options > Engine.\n"
            "History averages only compare games reviewed at the same depth and method.");
        note->SetForegroundColour(wxColour(100, 100, 100));
        top->Add(note, 0, wxALL, 10);
        top->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizerAndFit(top);
        CentreOnParent();
    }

    void Apply(wolf::ReviewSettings &s) const
    {
        int i = depth->GetSelection();
        s.depth = (i >= 0 && i < 4) ? kDepths[i] : (customDepth > 0 ? customDepth : 18);
        s.method = method->GetSelection() == 1 ? wolf::Method::Lichess : wolf::Method::SameRoot;
        s.recordHistory = record->GetValue();
    }
};

// ---------------------------------------------------------------- colours

wxColour judgementColour(wolf::Judgement j)
{
    switch (j) {
        case wolf::Judgement::Inaccuracy: return wxColour(214, 168, 0);
        case wolf::Judgement::Mistake:    return wxColour(230, 115, 0);
        case wolf::Judgement::Blunder:    return wxColour(205, 35, 35);
        default:                          return wxColour(70, 70, 70);
    }
}

wxColour judgementTint(wolf::Judgement j)
{
    switch (j) {
        case wolf::Judgement::Inaccuracy: return wxColour(255, 246, 205);
        case wolf::Judgement::Mistake:    return wxColour(255, 228, 196);
        case wolf::Judgement::Blunder:    return wxColour(255, 210, 210);
        default:                          return wxNullColour;
    }
}

const wxColour kBestColour(40, 140, 60);

const char *nagFor(wolf::Judgement j, char &nag)
{
    switch (j) {
        case wolf::Judgement::Inaccuracy: nag = 6; return "Inaccuracy";
        case wolf::Judgement::Mistake:    nag = 2; return "Mistake";
        case wolf::Judgement::Blunder:    nag = 4; return "Blunder";
        default:                          nag = 0; return nullptr;
    }
}

wxString moveLabel(const wolf::MoveReport &m)
{
    return wxString::Format(m.white ? "%d. %s" : "%d... %s", m.moveNumber, wxString::FromUTF8(m.san.c_str()));
}

// ---------------------------------------------------------------- eval graph

// Win chances over the game (white area = White is better), with a dot on every
// inaccuracy / mistake / blunder. Point 0 is the start position, point i+1 is the
// position after move i. Click or drag to pick a move.
class EvalGraph : public wxPanel
{
public:
    std::function<void(int)> onPick;   // move index, -1 = start position

    EvalGraph(wxWindow *parent, const wolf::GameReport &rep)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 150), wxBORDER_SIMPLE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        for (const wolf::Score &s : rep.evals)
            chances.push_back(wolf::winningChances(s.ceiledCp()));
        for (const wolf::MoveReport &m : rep.moves)
            marks.push_back(m.judgement);
        SetToolTip("Evaluation graph - click or drag to step through the game");
        Bind(wxEVT_PAINT, &EvalGraph::OnPaint, this);
        Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { Refresh(); e.Skip(); });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) { Pick(e.GetX()); });
        Bind(wxEVT_MOTION, [this](wxMouseEvent &e) { if (e.LeftIsDown()) Pick(e.GetX()); });
    }

    void SetSelected(int moveIdx) { if (moveIdx != selected) { selected = moveIdx; Refresh(); } }

private:
    std::vector<double> chances;
    std::vector<wolf::Judgement> marks;
    int selected = -2;
    int lastPicked = -3;
    static const int kMargin = 6;

    int points() const { return (int)chances.size(); }

    int xFor(int k, int w) const
    {
        int n = points() > 1 ? points() - 1 : 1;
        return kMargin + (int)((long long)k * (w - 2 * kMargin) / n);
    }
    int yFor(double c, int h) const
    {
        if (c > 1) c = 1;
        if (c < -1) c = -1;
        return (int)(h / 2.0 - c * (h / 2.0 - kMargin));
    }

    void Pick(int x)
    {
        int w = GetClientSize().GetWidth();
        int n = points() - 1;
        if (n < 1 || w <= 2 * kMargin)
            return;
        int k = (int)((double)(x - kMargin) * n / (w - 2 * kMargin) + 0.5);
        if (k < 0) k = 0;
        if (k > n) k = n;
        int moveIdx = k - 1;
        if (moveIdx == lastPicked && moveIdx == selected)
            return;
        lastPicked = moveIdx;
        SetSelected(moveIdx);
        if (onPick)
            onPick(moveIdx);
    }

    void OnPaint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        wxSize sz = GetClientSize();
        int w = sz.GetWidth(), h = sz.GetHeight();
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(wxColour(64, 64, 64)));        // Black's side
        dc.DrawRectangle(0, 0, w, h);
        if (points() < 2)
            return;

        std::vector<wxPoint> poly;
        poly.push_back(wxPoint(xFor(0, w), h));
        for (int k = 0; k < points(); ++k)
            poly.push_back(wxPoint(xFor(k, w), yFor(chances[k], h)));
        poly.push_back(wxPoint(xFor(points() - 1, w), h));
        dc.SetBrush(wxBrush(wxColour(236, 236, 236)));      // White's side
        dc.DrawPolygon((int)poly.size(), &poly[0]);

        dc.SetPen(wxPen(wxColour(140, 140, 140), 1, wxPENSTYLE_SHORT_DASH));
        dc.DrawLine(0, h / 2, w, h / 2);
        dc.SetPen(wxPen(wxColour(120, 120, 120), 1));
        for (int k = 1; k < points(); ++k)
            dc.DrawLine(poly[k], poly[k + 1]);

        if (selected >= -1 && selected + 1 < points()) {
            int x = xFor(selected + 1, w);
            dc.SetPen(wxPen(wxColour(40, 110, 220), 2));
            dc.DrawLine(x, 0, x, h);
        }

        for (size_t i = 0; i < marks.size() && i + 1 < chances.size(); ++i) {
            if (marks[i] == wolf::Judgement::None)
                continue;
            dc.SetPen(wxPen(*wxWHITE, 1));
            dc.SetBrush(wxBrush(judgementColour(marks[i])));
            dc.DrawCircle(xFor((int)i + 1, w), yFor(chances[i + 1], h), marks[i] == wolf::Judgement::Blunder ? 5 : 4);
        }
    }
};

// ---------------------------------------------------------------- review window

void addReviewToGame(GameLogic *gl, const wolf::GameReport &rep);

class ReviewWindow;
ReviewWindow *g_open = nullptr;   // one review window at a time

class ReviewWindow : public wxDialog
{
public:
    ReviewWindow(wxWindow *parent, GameLogic *gl_, const wolf::GameReport &rep_, const std::vector<thc::Move> &reviewed_,
                 const std::string &gameId_, const std::string &historyCsv_, const wxString &historyNote)
        : wxDialog(parent, wxID_ANY, "Game review", wxDefaultPosition, wxSize(900, 800),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX),
          gl(gl_), rep(rep_), reviewed(reviewed_), gameId(gameId_), historyCsv(historyCsv_)
    {
        history = wolf::readHistory(historyCsv);
        wxBoxSizer *top = new wxBoxSizer(wxVERTICAL);

        // Title line
        wxString title = wxString::FromUTF8((rep.white + "  vs  " + rep.black).c_str());
        if (!rep.result.empty() && rep.result != "*")
            title += "    " + wxString(rep.result.c_str());
        wxStaticText *t = new wxStaticText(this, wxID_ANY, title);
        t->SetFont(t->GetFont().Bold().Scaled(1.3f));
        top->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        wxString sub = wxString::Format("%s, depth %d, %s", wxString::FromUTF8(rep.engineName.c_str()), rep.depth,
                                        rep.method == wolf::Method::Lichess ? "Lichess-style judging" : "best-move judging");
        if (!rep.event.empty() && rep.event != "?")
            sub = wxString::FromUTF8(rep.event.c_str()) + "  -  " + sub;
        wxStaticText *s = new wxStaticText(this, wxID_ANY, sub);
        s->SetForegroundColour(wxColour(110, 110, 110));
        top->Add(s, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        // Player cards
        wxBoxSizer *cards = new wxBoxSizer(wxHORIZONTAL);
        cards->Add(PlayerCard("White", rep.white, rep.whiteStats), 1, wxEXPAND | wxRIGHT, 5);
        cards->Add(PlayerCard("Black", rep.black, rep.blackStats), 1, wxEXPAND | wxLEFT, 5);
        top->Add(cards, 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

        // Graph
        graph = new EvalGraph(this, rep);
        graph->onPick = [this](int moveIdx) { SelectMove(moveIdx, false); };
        top->Add(graph, 0, wxEXPAND | wxALL, 10);

        // Move list controls
        wxBoxSizer *bar = new wxBoxSizer(wxHORIZONTAL);
        wxButton *prev = new wxButton(this, wxID_ANY, "< Previous mistake");
        wxButton *next = new wxButton(this, wxID_ANY, "Next mistake >");
        onlyErrors = new wxCheckBox(this, wxID_ANY, "Only show inaccuracies, mistakes and blunders");
        bar->Add(prev, 0, wxRIGHT, 6);
        bar->Add(next, 0, wxRIGHT, 16);
        bar->Add(onlyErrors, 0, wxALIGN_CENTER_VERTICAL);
        top->Add(bar, 0, wxLEFT | wxRIGHT, 10);
        prev->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { HopMistake(-1); });
        next->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { HopMistake(+1); });
        onlyErrors->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { FillList(); });

        // Move list
        list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL);
        list->AppendColumn("Move", wxLIST_FORMAT_LEFT, 110);
        list->AppendColumn("Verdict", wxLIST_FORMAT_LEFT, 100);
        list->AppendColumn("Eval", wxLIST_FORMAT_RIGHT, 70);
        list->AppendColumn("Engine's best", wxLIST_FORMAT_LEFT, 120);
        list->AppendColumn("Loss", wxLIST_FORMAT_RIGHT, 60);
        list->AppendColumn("Accuracy", wxLIST_FORMAT_RIGHT, 80);
        list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &e) {
            long row = e.GetIndex();
            if (row >= 0 && row < (long)rowMove.size() && !filling)
                SelectMove(rowMove[row], true);
        });
        top->Add(list, 1, wxEXPAND | wxALL, 10);
        FillList();

        // Status + buttons
        status = new wxStaticText(this, wxID_ANY, historyNote.empty() ? wxString("Click a move or the graph to see it on the board.") : historyNote);
        status->SetForegroundColour(wxColour(110, 110, 110));
        top->Add(status, 0, wxLEFT | wxRIGHT, 10);
        wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
        wxButton *copy = new wxButton(this, wxID_ANY, "Copy report");
        wxButton *hist = new wxButton(this, wxID_ANY, "Open history");
        hist->SetToolTip(wxString::FromUTF8(historyCsv.c_str()));
        hist->Enable(wxFileExists(wxString(historyCsv.c_str(), wxConvLibc)));
        buttons->Add(hist, 0, wxRIGHT, 8);
        hist->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (!wxLaunchDefaultApplication(wxString(historyCsv.c_str(), wxConvLibc)))
                status->SetLabel("Couldn't open " + wxString(historyCsv.c_str(), wxConvLibc));
        });
        addBtn = new wxButton(this, wxID_ANY, "Add review to game");
        addBtn->SetToolTip("Marks the main line with ?! / ? / ?? and 'Best was ...' comments, plus a summary at the end (undo-able)");
        wxButton *close = new wxButton(this, wxID_CLOSE, "Close");
        buttons->Add(copy, 0, wxRIGHT, 8);
        buttons->Add(addBtn, 0, wxRIGHT, 8);
        buttons->Add(close, 0);
        top->Add(buttons, 0, wxALIGN_RIGHT | wxALL, 10);
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { CopyReport(); });
        addBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { AddToGame(); });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); });
        SetEscapeId(wxID_CLOSE);

        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &) {
            if (g_open == this)
                g_open = nullptr;
            Destroy();
        });

        SetSizer(top);
        SetMinSize(wxSize(640, 560));
        CentreOnParent();
    }

private:
    GameLogic *gl;
    wolf::GameReport rep;
    std::vector<thc::Move> reviewed;
    std::string gameId, historyCsv;
    std::vector<wolf::HistoryRow> history;
    EvalGraph *graph = nullptr;
    wxListCtrl *list = nullptr;
    wxCheckBox *onlyErrors = nullptr;
    wxStaticText *status = nullptr;
    wxButton *addBtn = nullptr;
    std::vector<int> rowMove;     // list row -> move index
    int current = -2;
    bool filling = false;

    wxSizer *PlayerCard(const wxString &side, const std::string &name, const wolf::PlayerStats &p)
    {
        wxStaticBoxSizer *box = new wxStaticBoxSizer(wxVERTICAL, this, side + ":  " + wxString::FromUTF8(name.c_str()));
        wxWindow *sb = box->GetStaticBox();
        if (!p.hasAccuracy || p.moves == 0) {
            box->Add(new wxStaticText(sb, wxID_ANY, "No moves to judge."), 0, wxALL, 8);
            return box;
        }
        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText *acc = new wxStaticText(sb, wxID_ANY, wxString::Format("%.1f%%", p.accuracy));
        acc->SetFont(acc->GetFont().Bold().Scaled(2.2f));
        row->Add(acc, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 14);

        wxBoxSizer *col = new wxBoxSizer(wxVERTICAL);
        col->Add(new wxStaticText(sb, wxID_ANY, "accuracy"), 0);
        wolf::EloEstimate e = wolf::estimateElo(p);
        if (e.valid) {
            wxStaticText *elo = new wxStaticText(sb, wxID_ANY,
                wxString::Format("Played like ~%d Elo  (likely %d-%d)%s", e.elo, e.low, e.high,
                                 e.veryRough ? "  - short game, very rough" : ""));
            elo->SetFont(elo->GetFont().Bold());
            col->Add(elo, 0, wxTOP, 2);
        }
        col->Add(new wxStaticText(sb, wxID_ANY, wxString::Format("Average loss %.0f centipawns over %d moves", p.acpl, p.moves)), 0, wxTOP, 2);
        row->Add(col, 1, wxALIGN_CENTER_VERTICAL);
        box->Add(row, 0, wxEXPAND | wxALL, 6);

        wxBoxSizer *counts = new wxBoxSizer(wxHORIZONTAL);
        auto count = [&](const wxString &label, int n, const wxColour &c) {
            wxStaticText *st = new wxStaticText(sb, wxID_ANY, wxString::Format("%d %s", n, label));
            st->SetForegroundColour(c);
            st->SetFont(st->GetFont().Bold());
            counts->Add(st, 0, wxRIGHT, 14);
        };
        count("best", p.best, kBestColour);
        count("inaccuracies", p.inaccuracies, judgementColour(wolf::Judgement::Inaccuracy));
        count("mistakes", p.mistakes, judgementColour(wolf::Judgement::Mistake));
        count("blunders", p.blunders, judgementColour(wolf::Judgement::Blunder));
        box->Add(counts, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

        // Earlier games by this player, reviewed the same way (same depth, method and engine)
        wolf::HistoryRow like;
        like.depth = rep.depth;
        like.method = wolf::methodKey(rep.method);
        like.engine = rep.engineName;
        wolf::PlayerHistory h = wolf::summarizePlayer(history, name, like, gameId);
        wxString line;
        if (h.games == 0)
            line = wxString::Format("No earlier games in your history at depth %d with this method.", rep.depth);
        else {
            line = wxString::Format("Earlier: %d game%s at depth %d, average %.1f%%", h.games, h.games == 1 ? "" : "s",
                                    rep.depth, h.avgAccuracy);
            if (h.avgElo > 0)
                line += wxString::Format(", ~%d Elo (%d-%d)", (int)(h.avgElo + 0.5), h.worstElo, h.bestElo);
            double diff = p.accuracy - h.avgAccuracy;
            line += wxString::Format(".  This game: %s%.1f", diff >= 0 ? "+" : "", diff);
        }
        wxStaticText *ht = new wxStaticText(sb, wxID_ANY, line);
        ht->SetForegroundColour(wxColour(90, 90, 140));
        box->Add(ht, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
        return box;
    }

    void FillList()
    {
        filling = true;
        list->Freeze();
        list->DeleteAllItems();
        rowMove.clear();
        bool only = onlyErrors && onlyErrors->GetValue();
        for (size_t i = 0; i < rep.moves.size(); ++i) {
            const wolf::MoveReport &m = rep.moves[i];
            if (only && m.judgement == wolf::Judgement::None)
                continue;
            long row = list->InsertItem(list->GetItemCount(), moveLabel(m));
            wxString verdict = m.judgement != wolf::Judgement::None ? wxString(wolf::judgementName(m.judgement))
                                                                    : (m.isBest ? wxString("Best") : wxString());
            list->SetItem(row, 1, verdict);
            list->SetItem(row, 2, wxString(m.after.str().c_str()));
            list->SetItem(row, 3, m.isBest ? wxString("-") : wxString::FromUTF8(m.bestSan.c_str()));
            list->SetItem(row, 4, m.cpl > 0 ? wxString::Format("%d", m.cpl) : wxString("0"));
            list->SetItem(row, 5, wxString::Format("%.0f%%", m.accuracy));
            if (m.judgement != wolf::Judgement::None) {
                list->SetItemBackgroundColour(row, judgementTint(m.judgement));
                list->SetItemTextColour(row, judgementColour(m.judgement).ChangeLightness(70));
            } else if (m.isBest) {
                list->SetItemTextColour(row, kBestColour);
            }
            rowMove.push_back((int)i);
        }
        list->Thaw();
        filling = false;
        HighlightRow(current);
    }

    void HighlightRow(int moveIdx)
    {
        filling = true;
        long sel = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel >= 0)
            list->SetItemState(sel, 0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        for (size_t r = 0; r < rowMove.size(); ++r) {
            if (rowMove[r] == moveIdx) {
                list->SetItemState((long)r, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                   wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                list->EnsureVisible((long)r);
                break;
            }
        }
        filling = false;
    }

    void SelectMove(int moveIdx, bool fromList)
    {
        current = moveIdx;
        graph->SetSelected(moveIdx);
        if (!fromList)
            HighlightRow(moveIdx);
        ShowOnBoard(moveIdx);
    }

    void HopMistake(int dir)
    {
        int n = (int)rep.moves.size();
        int i = current < -1 ? (dir > 0 ? -1 : n) : current;
        for (i += dir; i >= 0 && i < n; i += dir) {
            if (rep.moves[i].judgement != wolf::Judgement::None) {
                SelectMove(i, false);
                return;
            }
        }
        status->SetLabel(dir > 0 ? "No more mistakes after this move." : "No mistakes before this move.");
    }

    // The review only lines up with the game if the main line still starts with the reviewed moves.
    bool GameMatches()
    {
        if (gl->gd.tree.variations.empty())
            return false;
        VARIATION &line = gl->gd.tree.variations[0];
        if (line.size() < reviewed.size())
            return false;
        for (size_t i = 0; i < reviewed.size(); ++i)
            if (!(line[i].game_move.move == reviewed[i]))
                return false;
        return true;
    }

    void ShowOnBoard(int moveIdx)
    {
        if (!GameMatches()) {
            status->SetLabel("The game on the board has changed since this review, so it can't jump there. Run the review again.");
            return;
        }
        Atomic begin(false);
        unsigned long pos;
        if (moveIdx < 0)
            pos = gl->gd.gv.FindMove0();
        else
            pos = gl->gd.gv.GetMoveOffset(&gl->gd.tree.variations[0][moveIdx]);
        gl->atom.Redisplay(pos);
        if (moveIdx < 0)
            status->SetLabel("Start position.");
        else {
            const wolf::MoveReport &m = rep.moves[moveIdx];
            wxString txt = "After " + moveLabel(m) + "  (" + wxString(m.after.str().c_str()) + ")";
            if (!m.isBest)
                txt += "  -  engine preferred " + wxString::FromUTF8(m.bestSan.c_str()) + " (" + wxString(m.before.str().c_str()) + ")";
            status->SetLabel(txt);
        }
    }

    void CopyReport()
    {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(wxString::FromUTF8(wolf::formatReport(rep).c_str())));
            wxTheClipboard->Close();
            status->SetLabel("Full text review copied to the clipboard.");
        }
    }

    void AddToGame()
    {
        if (!GameMatches()) {
            wxMessageBox("The game on the board has changed since this review. Run the review again first.",
                         "Game review", wxOK | wxICON_INFORMATION, this);
            return;
        }
        addReviewToGame(gl, rep);
        addBtn->Disable();
        addBtn->SetLabel("Added to game");
        status->SetLabel("Review added to the game (Edit > Undo removes it).");
    }
};

void addReviewToGame(GameLogic *gl, const wolf::GameReport &rep)
{
    Atomic begin;
    if (gl->gd.tree.variations.empty())
        return;
    VARIATION &line = gl->gd.tree.variations[0];
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
    gl->gd.Rebuild();
    gl->atom.Undo();
    gl->atom.Redisplay(gl->gd.gv.FindEnd());
    gl->atom.StatusUpdate();
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

    std::string iniPath = settingsPath();
    wolf::ReviewSettings cfg;
    wolf::loadSettings(iniPath, cfg);
    ReviewSettingsDialog settings(objs.frame, cfg);
    if (settings.ShowModal() != wxID_OK)
        return;
    settings.Apply(cfg);
    wolf::saveSettings(iniPath, cfg);

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

    // Remember exactly which moves were reviewed, so the window can tell if the game changes
    std::vector<thc::Move> reviewed;
    if (!gd.tree.variations.empty())
        for (const MoveTree &mt : gd.tree.variations[0])
            reviewed.push_back(mt.game_move.move);

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
    opt.depth = cfg.depth;
    opt.method = cfg.method;
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
    if (reviewed.size() > rep.moves.size())
        reviewed.resize(rep.moves.size());

    // Review history (CSV): one row per player, tagged with depth / method / engine
    std::string gameId = wolf::gameFingerprint(games[0]);
    std::string csv = wolf::historyPath(iniPath, cfg);
    wxString note;
    if (cfg.recordHistory) {
        std::string herr;
        if (wolf::recordHistory(csv, wolf::historyRows(rep, gameId, wolf::nowStamp()), herr))
            note = "Saved to your review history.  Click a move or the graph to see it on the board.";
        else
            note = "Not saved to history: " + wxString(herr.c_str(), wxConvLibc);
    }

    if (g_open)
        g_open->Close();
    g_open = new ReviewWindow(objs.frame, this, rep, reviewed, gameId, csv, note);
    g_open->Show();
}
