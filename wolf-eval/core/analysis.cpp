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
    std::vector<bool> skip;
    for (MoveReport &m : rep.moves) {
        int b = m.before.ceiledCp(), a = m.after.ceiledCp();
        int sign = m.white ? 1 : -1;
        m.cpl = std::max(0, sign * (b - a));
        double wb = winPercent(b), wa = winPercent(a);
        m.accuracy = m.white ? moveAccuracy(wb, wa) : moveAccuracy(100 - wb, 100 - wa);
        m.judgement = judge(b, a, m.white);
        acc.push_back(m.accuracy);
        skip.push_back(m.book);
        PlayerStats &p = *ps[m.white ? 0 : 1];
        if (m.book) {             // theory: shown, never punished, not counted
            m.judgement = Judgement::None;
            m.special = Special::None;
            p.bookMoves++;
            continue;
        }
        if (m.special == Special::Brilliant) p.brilliant++;
        else if (m.special == Special::Great) p.great++;
        p.moves++;
        if (m.isBest) p.best++;
        if (m.judgement == Judgement::Inaccuracy) p.inaccuracies++;
        if (m.judgement == Judgement::Mistake) p.mistakes++;
        if (m.judgement == Judgement::Blunder) p.blunders++;
        cplSum[m.white ? 0 : 1] += m.cpl;
    }
    for (int s = 0; s < 2; ++s)
        if (ps[s]->moves) ps[s]->acpl = double(cplSum[s]) / ps[s]->moves;

    GameAccuracy g = gameAccuracy(cps, whiteStarts, acc, skip);
    rep.whiteStats.hasAccuracy = g.hasWhite; rep.whiteStats.accuracy = g.white;
    rep.blackStats.hasAccuracy = g.hasBlack; rep.blackStats.accuracy = g.black;
}

bool applyGapRule(GameReport &rep, const OpeningBook &book, const std::string &startFen,
                  const std::vector<std::string> &sanMoves) {
    OpeningInfo &op = rep.opening;
    if (!op.gap || op.gapFirstPly < 1) return false;
    bool weak = false;
    for (int p = op.gapFirstPly; p <= op.gapLastPly && p <= (int)rep.moves.size(); ++p) {
        const MoveReport &m = rep.moves[p - 1];
        if (judge(m.before.ceiledCp(), m.after.ceiledCp(), m.white) != Judgement::None) weak = true;
    }
    if (!weak) return false;

    std::vector<std::string> prefix(sanMoves.begin(), sanMoves.begin() + std::min<size_t>(op.gapFirstPly - 1, sanMoves.size()));
    OpeningInfo pre = classifyOpening(book, startFen, prefix);
    pre.gapRejected = true;
    pre.bookEndPly = op.gapFirstPly - 1;
    if (pre.bookEndPly > 0 && pre.bookEndPly <= (int)sanMoves.size()) pre.bookEndSan = sanMoves[pre.bookEndPly - 1];
    pre.droppedEco = op.eco;
    pre.droppedName = op.name;
    pre.droppedReentryPly = op.reentryPly;
    pre.gapFirstPly = op.gapFirstPly;
    pre.gapLastPly = op.gapLastPly;
    pre.reentryPly = op.reentryPly;
    pre.leftBookPly = op.gapFirstPly;
    pre.bookMove.resize(rep.moves.size(), false);
    pre.games.resize(rep.moves.size(), 0);
    pre.bookPlies = 0;
    for (bool b : pre.bookMove) if (b) ++pre.bookPlies;
    op = pre;
    for (size_t i = 0; i < rep.moves.size(); ++i) {
        rep.moves[i].book = op.bookMove[i];
        rep.moves[i].bookGames = op.games[i];
    }
    return true;
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
    if (opt.book) {
        rep.opening = classifyOpening(*opt.book, fen, game.sanMoves);
        rep.bookVersion = opt.book->version;
    }

    rep.method = opt.method;
    engine.setOption("MultiPV", opt.findSpecial ? "2" : "1");
    engine.newGame();
    thc::ChessRules pos = cr;
    std::vector<std::string> bestUci(fens.size());
    std::vector<Score> playedScore(played.size());
    std::vector<Score> secondScore(fens.size());
    std::vector<bool> hasSecond(fens.size(), false);
    std::vector<std::vector<std::string>> playedLine(played.size());
    std::vector<std::vector<std::string>> posLine(fens.size());   // engine line from each position
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
            posLine[i] = er.pv;
            if (er.hasSecond) {
                EngineResult two = er;
                two.isMate = er.secondIsMate;
                two.value = er.secondValue;
                secondScore[i] = toWhite(two);
                hasSecond[i] = true;
            }
            if (opt.method == Method::SameRoot && i < played.size()) {
                std::string mine = played[i].TerseOut();
                if (mine == er.bestMoveUci) { playedScore[i] = s; playedLine[i] = er.pv; }
                else {
                    EngineResult pr = engine.analyse(fens[i], opt.depth, opt.movetimeMs, mine);
                    if (!pr.ok) { rep.error = "The engine stopped answering at move " + std::to_string(i / 2 + firstMoveNumber) + "."; return rep; }
                    playedScore[i] = toWhite(pr);
                    playedLine[i] = pr.pv;
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
        if (i < rep.opening.bookMove.size()) {
            m.book = rep.opening.bookMove[i];
            m.bookGames = rep.opening.games[i];
        }
        rep.moves.push_back(m);
        r.PlayMove(played[i]);
    }
    if (opt.book) applyGapRule(rep, *opt.book, fen, game.sanMoves);

    if (opt.findSpecial) {
        thc::ChessRules q = cr;
        std::string prevUci;
        bool prevCapture = false;
        // The rest of the line of the latest special move, while the game keeps following it:
        // the mover's next steps of that combination aren't judged as new "only moves".
        std::vector<std::string> plan;
        bool planWhite = false;
        for (size_t i = 0; i < played.size(); ++i) {
            std::string uci = played[i].TerseOut();
            std::string to = uci.substr(2, 2);
            int idx = (7 - (to[1] - '1')) * 8 + (to[0] - 'a');
            bool capture = q.squares[idx] != ' ' && q.squares[idx] != 0;
            if (!capture && (q.squares[(7 - (uci[1] - '1')) * 8 + (uci[0] - 'a')] == 'P' ||
                             q.squares[(7 - (uci[1] - '1')) * 8 + (uci[0] - 'a')] == 'p') && uci[0] != uci[2])
                capture = true;   // en passant
            bool moverWhite = q.WhiteToPlay();
            bool followsUp = false;
            if (!plan.empty() && plan.front() == uci) {
                followsUp = moverWhite == planWhite;
                plan.erase(plan.begin());
            } else {
                plan.clear();
            }
            if (!bestUci[i].empty()) {
                SpecialInput si;
                si.fenBefore = fens[i];
                si.playedUci = uci;
                si.line = playedLine[i];
                if (si.line.empty()) {   // Lichess method: the move, then the engine's line from the next position
                    si.line.push_back(uci);
                    si.line.insert(si.line.end(), posLine[i + 1].begin(), posLine[i + 1].end());
                }
                si.best = rep.evals[i];
                si.played = rep.moves[i].after;
                si.hasSecond = hasSecond[i];
                si.second = secondScore[i];
                si.isBest = rep.moves[i].isBest;
                si.prevMoveUci = prevUci;
                si.prevWasCapture = prevCapture;
                si.book = rep.moves[i].book;
                si.followsUp = followsUp;
                SpecialResult sr = judgeSpecial(si);
                rep.moves[i].special = sr.kind;
                rep.moves[i].specialWhy = sr.why;
                if (sr.kind != Special::None && si.line.size() > 1) {
                    plan.assign(si.line.begin() + 1, si.line.end());
                    planWhite = moverWhite;
                }
            }
            prevUci = uci;
            prevCapture = capture;
            q.PlayMove(played[i]);
        }
    }
    scoreGame(rep, whiteStarts);
    rep.ok = true;
    return rep;
}

} // namespace wolf
