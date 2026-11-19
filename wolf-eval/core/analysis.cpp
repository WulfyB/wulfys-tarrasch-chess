#include <cstdint>
#include <string>
#include "analysis.h"
#include "thc.h"
#include <algorithm>

namespace wolf {

static std::string normaliseSan(std::string s) {
    for (char &c : s) if (c == '0') c = 'O';             // 0-0 -> O-O (no SAN square contains '0')
    while (!s.empty() && (s.back() == '+' || s.back() == '#')) s.pop_back();
    return s;
}

void scoreGame(GameReport &rep, bool whiteStarts) {
    std::vector<int> cps;
    for (auto &e : rep.evals) cps.push_back(e.ceiledCp());

    PlayerStats *ps[2] = {&rep.whiteStats, &rep.blackStats};
    long cplSum[2] = {0, 0};
    std::vector<double> acc;
    for (MoveReport &m : rep.moves) {
        int b = m.before.ceiledCp(), a = m.after.ceiledCp();
        int sign = m.white ? 1 : -1;
        m.cpl = std::max(0, sign * (b - a));
        double wb = winPercent(b), wa = winPercent(a);
        m.accuracy = m.white ? moveAccuracy(wb, wa) : moveAccuracy(100 - wb, 100 - wa);
        m.judgement = judge(b, a, m.white);
        acc.push_back(m.accuracy);
        PlayerStats &p = *ps[m.white ? 0 : 1];
        p.moves++;
        if (m.isBest) p.best++;
        if (m.judgement == Judgement::Inaccuracy) p.inaccuracies++;
        if (m.judgement == Judgement::Mistake) p.mistakes++;
        if (m.judgement == Judgement::Blunder) p.blunders++;
        cplSum[m.white ? 0 : 1] += m.cpl;
    }
    for (int s = 0; s < 2; ++s)
        if (ps[s]->moves) ps[s]->acpl = double(cplSum[s]) / ps[s]->moves;

    GameAccuracy g = gameAccuracy(cps, whiteStarts, acc);
    rep.whiteStats.hasAccuracy = g.hasWhite; rep.whiteStats.accuracy = g.white;
    rep.blackStats.hasAccuracy = g.hasBlack; rep.blackStats.accuracy = g.black;
}

GameReport analyseGame(const PgnGame &game, UciEngine &engine, const AnalysisOptions &opt) {
    GameReport rep;
    rep.white = game.header("White"); rep.black = game.header("Black");
    rep.result = game.header("Result", "*"); rep.event = game.header("Event");
    rep.date = game.header("Date"); rep.whiteElo = game.header("WhiteElo", "");
    rep.blackElo = game.header("BlackElo", "");
    rep.engineName = engine.name();
    rep.depth = opt.depth; rep.movetimeMs = opt.movetimeMs;

    thc::ChessRules cr;
    std::string fen = game.header("FEN", "");
    if (!fen.empty() && !cr.Forsyth(fen.c_str())) { rep.error = "Bad FEN header: " + fen; return rep; }
    bool whiteStarts = cr.WhiteToPlay();
    int firstMoveNumber = cr.full_move_count > 0 ? cr.full_move_count : 1;

    // Replay the game first so an illegal move is reported before any engine time is spent.
    std::vector<std::string> fens;
    std::vector<thc::Move> played;
    {
        thc::ChessRules r = cr;
        for (size_t i = 0; i < game.sanMoves.size(); ++i) {
            fens.push_back(r.ForsythPublish());
            thc::Move mv;
            std::string san = normaliseSan(game.sanMoves[i]);
            if (!mv.NaturalIn(&r, san.c_str())) {
                rep.error = "Could not read move " + std::to_string(i / 2 + firstMoveNumber) + ": '" + game.sanMoves[i] + "'";
                return rep;
            }
            played.push_back(mv);
            r.PlayMove(mv);
        }
        fens.push_back(r.ForsythPublish());
    }
    if (played.empty()) { rep.error = "The game has no moves."; return rep; }

    rep.method = opt.method;
    engine.newGame();
    thc::ChessRules pos = cr;
    std::vector<std::string> bestUci(fens.size());
    std::vector<Score> playedScore(played.size());
    for (size_t i = 0; i < fens.size(); ++i) {
        thc::TERMINAL term = thc::NOT_TERMINAL;
        pos.Evaluate(term);
        bool whiteToMove = pos.WhiteToPlay();
        auto toWhite = [&](const EngineResult &er) {
            int v = whiteToMove ? er.value : -er.value;
            if (er.isMate && er.value == 0) v = whiteToMove ? -1 : 1;   // "mate 0": side to move is mated
            return er.isMate ? Score::Mate(v) : Score::Cp(v);
        };
        Score s;
        if (term == thc::TERMINAL_WCHECKMATE) s = Score::Cp(-CP_CEILING);
        else if (term == thc::TERMINAL_BCHECKMATE) s = Score::Cp(CP_CEILING);
        else if (term == thc::TERMINAL_WSTALEMATE || term == thc::TERMINAL_BSTALEMATE) s = Score::Cp(0);
        else {
            EngineResult er = engine.analyse(fens[i], opt.depth, opt.movetimeMs);
            if (!er.ok) { rep.error = "The engine stopped answering at move " + std::to_string(i / 2 + firstMoveNumber) + "."; return rep; }
            s = toWhite(er);
            bestUci[i] = er.bestMoveUci;
            if (opt.method == Method::SameRoot && i < played.size()) {
                std::string mine = played[i].TerseOut();
                if (mine == er.bestMoveUci) playedScore[i] = s;
                else {
                    EngineResult pr = engine.analyse(fens[i], opt.depth, opt.movetimeMs, mine);
                    if (!pr.ok) { rep.error = "The engine stopped answering at move " + std::to_string(i / 2 + firstMoveNumber) + "."; return rep; }
                    playedScore[i] = toWhite(pr);
                    // A deeper look at one move can't be better than the best move: keep them consistent.
                    int sign = whiteToMove ? 1 : -1;
                    if (sign * playedScore[i].ceiledCp() > sign * s.ceiledCp()) s = playedScore[i];
                }
            }
        }
        rep.evals.push_back(s);
        if (i < played.size()) pos.PlayMove(played[i]);
        if (opt.progress && !opt.progress(int(i + 1), int(fens.size()))) { rep.error = "Cancelled."; return rep; }
    }

    thc::ChessRules r = cr;
    for (size_t i = 0; i < played.size(); ++i) {
        MoveReport m;
        m.ply = int(i) + 1;
        m.white = r.WhiteToPlay();
        m.moveNumber = r.full_move_count;
        m.san = played[i].NaturalOut(&r);
        m.before = rep.evals[i];
        m.after = (opt.method == Method::SameRoot && !bestUci[i].empty()) ? playedScore[i] : rep.evals[i + 1];
        if (!bestUci[i].empty()) {
            thc::Move bm;
            if (bm.TerseIn(&r, bestUci[i].c_str())) {
                m.bestSan = bm.NaturalOut(&r);
                m.isBest = (bm == played[i]);
            }
        }
        rep.moves.push_back(m);
        r.PlayMove(played[i]);
    }
    scoreGame(rep, whiteStarts);
    rep.ok = true;
    return rep;
}

} // namespace wolf
