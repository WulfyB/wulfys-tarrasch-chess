// Unit tests for the Wolf Eval core. Run with no arguments for the test suite, or
//   wolf-tests --game-accuracy W|B cp0 cp1 ...   to print "white black" (used by the Python cross-check).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "accuracy.h"
#include "analysis.h"
#include "elo.h"
#include "report.h"
#include "history.h"
#include "opening.h"
#include <cstdio>
#include "pgn.h"
#include "thc.h"

using namespace wolf;
static int failures = 0, checks = 0;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void testWinPercent() {
    CHECK(near(winPercent(0), 50));
    CHECK(winPercent(100) > 58 && winPercent(100) < 60);    // ~59.1
    CHECK(near(winPercent(300) + winPercent(-300), 100));
    CHECK(near(winPercent(5000), winPercent(1000)));          // capped at 10 pawns
    CHECK(near(winPercent(-5000), winPercent(-1000)));
}

static void testMoveAccuracy() {
    CHECK(near(moveAccuracy(50, 50), 100));
    CHECK(near(moveAccuracy(50, 60), 100));                   // improving is perfect
    double a10 = moveAccuracy(60, 50);
    CHECK(a10 > 60 && a10 < 70);                              // curve fit point: diff 10 -> ~60 (+1 bonus)
    CHECK(moveAccuracy(100, 0) == 0);
    CHECK(moveAccuracy(70, 69) > 96);
}

static void testJudge() {
    CHECK(judge(0, 0, true) == Judgement::None);
    CHECK(judge(0, -1000, true) == Judgement::Blunder);
    CHECK(judge(0, 1000, false) == Judgement::Blunder);       // Black let White win
    CHECK(judge(0, 1000, true) == Judgement::None);           // White improved
    // Find the cp drop at each threshold and make sure categories are ordered.
    Judgement prev = Judgement::None; bool ordered = true;
    for (int d = 0; d <= 600; d += 5) {
        Judgement j = judge(0, -d, true);
        if (int(j) < int(prev)) ordered = false;
        prev = j;
    }
    CHECK(ordered);
}

static void testGameAccuracy() {
    // A game where nobody loses anything: both 100.
    std::vector<int> flat(41, 20);
    GameAccuracy g = gameAccuracy(flat, true);
    CHECK(g.hasWhite && g.hasBlack);
    CHECK(near(g.white, 100) && near(g.black, 100));
    // White blunders once: White's accuracy drops, Black stays 100.
    std::vector<int> b(41, 20);
    for (size_t i = 21; i < b.size(); ++i) b[i] = -600;
    g = gameAccuracy(b, true);
    CHECK(g.white < 95 && near(g.black, 100));
    // Too short to score.
    CHECK(!gameAccuracy({0}, true).hasWhite);
}

static void testPgn() {
    const char *pgn =
        "[Event \"Test\"]\n[White \"Wulfy\"]\n[Black \"Bot\"]\n[Result \"1-0\"]\n\n"
        "1. e4 {best by test} e5 2. Nf3!? (2. f4 exf4) Nc6 $1 3.Bb5 a6 4. Ba4 Nf6 5. O-O Be7 1-0\n\n"
        "[Event \"Second\"]\n\n1. d4 d5 2. c4 *\n";
    auto games = parsePgn(pgn);
    CHECK(games.size() == 2);
    CHECK(games[0].header("White") == "Wulfy");
    CHECK(games[0].sanMoves.size() == 10);
    CHECK(games[0].sanMoves[2] == "Nf3");
    CHECK(games[0].sanMoves[4] == "Bb5");
    CHECK(games[0].sanMoves[8] == "O-O");
    CHECK(games[1].sanMoves.size() == 3);

    // Every SAN must be legal when replayed with Tarrasch's rules.
    thc::ChessRules cr;
    bool legal = true;
    for (auto &s : games[0].sanMoves) { thc::Move m; if (!m.NaturalIn(&cr, s.c_str())) legal = false; else cr.PlayMove(m); }
    CHECK(legal);
}

static void testScoreGame() {
    GameReport r;
    for (int cp : {20, 30, 25, -400, -380}) r.evals.push_back(Score::Cp(cp));
    for (int i = 0; i < 4; ++i) {
        MoveReport m; m.ply = i + 1; m.white = (i % 2 == 0);
        m.before = r.evals[i]; m.after = r.evals[i + 1];          // Lichess-style pairing
        r.moves.push_back(m);
    }
    scoreGame(r, true);
    CHECK(r.moves[0].cpl == 0);                 // White improved 20 -> 30
    CHECK(r.moves[1].cpl == 0);                 // Black: 30 -> 25 is a gain for Black
    CHECK(r.moves[2].cpl == 425);               // White: 25 -> -400
    CHECK(r.moves[2].judgement == Judgement::Blunder);
    CHECK(r.whiteStats.blunders == 1 && r.blackStats.blunders == 0);
    CHECK(near(r.whiteStats.acpl, 212.5));
    // Lichess-style pairing must reproduce the plain gameAccuracy() result exactly.
    GameAccuracy g = gameAccuracy({20, 30, 25, -400, -380}, true);
    CHECK(near(r.whiteStats.accuracy, g.white) && near(r.blackStats.accuracy, g.black));

    // Same-root pairing: White's best was +0.30 but the move played is worth -0.50.
    GameReport s;
    for (int cp : {30, -50}) s.evals.push_back(Score::Cp(cp));
    MoveReport m; m.ply = 1; m.white = true; m.before = Score::Cp(30); m.after = Score::Cp(-50);
    s.moves.push_back(m);
    scoreGame(s, true);
    CHECK(s.moves[0].cpl == 80);
    CHECK(s.moves[0].accuracy < 100);
}

static void testElo() {
    // monotonic: better accuracy / lower loss never lowers the estimate
    for (int a = 0; a < 100; ++a) CHECK(eloFromAccuracy(a + 1) >= eloFromAccuracy(a));
    for (int c = 0; c < 200; ++c) CHECK(eloFromAcpl(c + 1) <= eloFromAcpl(c));
    CHECK(eloFromAcpl(20) > 2300 && eloFromAcpl(20) < 2550);
    CHECK(eloFromAcpl(100) > 800 && eloFromAcpl(100) < 1100);
    PlayerStats strong; strong.hasAccuracy = true; strong.accuracy = 94; strong.acpl = 15; strong.moves = 40;
    PlayerStats weak;   weak.hasAccuracy = true;   weak.accuracy = 62;   weak.acpl = 85; weak.moves = 40;
    EloEstimate es = estimateElo(strong), ew = estimateElo(weak);
    CHECK(es.valid && ew.valid);
    CHECK(es.elo > 2400 && es.elo < 2800);
    CHECK(ew.elo > 900 && ew.elo < 1300);
    CHECK(es.low < es.elo && es.elo < es.high);
    PlayerStats shortG = strong; shortG.moves = 10;
    EloEstimate esh = estimateElo(shortG);
    CHECK(esh.veryRough && (esh.high - esh.low) > (es.high - es.low));
    PlayerStats none; CHECK(!estimateElo(none).valid);
    GameReport r; r.white = "Morphy"; r.black = "Allies"; r.whiteStats = strong; r.blackStats = weak;
    std::string txt = formatReport(r);
    CHECK(txt.find("Morphy") != std::string::npos);
    CHECK(txt.find("Played like roughly") != std::string::npos);
}


static void testHistory() {
    // Works in the current folder (portable); cleans up after itself.
    std::string ini = "wolf_test_WolfReview.ini";
    std::remove(ini.c_str()); std::remove("WolfReview history.csv");
    ReviewSettings s;
    CHECK(loadSettings(ini, s) && s.depth == 18 && s.recordHistory);   // missing file = defaults
    s.depth = 22; s.method = Method::Lichess; s.recordHistory = false;
    CHECK(saveSettings(ini, s));
    ReviewSettings t;
    CHECK(loadSettings(ini, t) && t.depth == 22 && t.method == Method::Lichess && !t.recordHistory);
    CHECK(historyPath(ini, t) == "WolfReview history.csv");

    PgnGame g; g.headers["White"] = "Wulfy, The"; g.headers["Black"] = "Bot \"X\""; g.sanMoves = {"e4", "e5"};
    PgnGame g2 = g; g2.sanMoves.push_back("Nf3");
    CHECK(gameFingerprint(g) == gameFingerprint(g) && gameFingerprint(g) != gameFingerprint(g2));

    GameReport r; r.white = "Wulfy, The"; r.black = "Bot \"X\""; r.depth = 18; r.engineName = "Stockfish 17";
    r.whiteStats.hasAccuracy = true; r.whiteStats.accuracy = 85.25; r.whiteStats.acpl = 30; r.whiteStats.moves = 30;
    r.blackStats.hasAccuracy = true; r.blackStats.accuracy = 70; r.blackStats.acpl = 60; r.blackStats.moves = 30;
    std::string csv = historyPath(ini, t), err;
    std::vector<HistoryRow> rows = historyRows(r, gameFingerprint(g), "2026-11-19 15:00");
    CHECK(rows.size() == 2 && rows[0].color == "white" && rows[1].player == "Bot \"X\"" && rows[0].estElo > 0);
    CHECK(recordHistory(csv, rows, err));
    CHECK(recordHistory(csv, rows, err));                        // same game, same setup: replaced
    CHECK(readHistory(csv).size() == 2);
    GameReport deep = r; deep.depth = 22;                         // same game, deeper: kept separately
    CHECK(recordHistory(csv, historyRows(deep, gameFingerprint(g), "2026-11-19 16:00"), err));
    GameReport other = r; other.whiteStats.accuracy = 95.25;
    CHECK(recordHistory(csv, historyRows(other, gameFingerprint(g2), "2026-11-19 17:00"), err));
    std::vector<HistoryRow> all = readHistory(csv);
    CHECK(all.size() == 6);
    CHECK(all[1].player == "Bot \"X\"" && all[0].white == "Wulfy, The" && near(all[0].accuracy, 85.25, 0.06));
    HistoryRow like = rows[0];
    PlayerHistory h = summarizePlayer(all, " wulfy, the ", like, "");
    CHECK(h.games == 2 && near(h.avgAccuracy, (85.25 + 95.25) / 2, 0.11));   // depth-22 row not mixed in
    CHECK(summarizePlayer(all, "Wulfy, The", like, gameFingerprint(g2)).games == 1);
    HistoryRow likeDeep = like; likeDeep.depth = 22;
    CHECK(summarizePlayer(all, "Wulfy, The", likeDeep, "").games == 1);
    HistoryRow likeOther = like; likeOther.engine = "Komodo";
    CHECK(summarizePlayer(all, "Wulfy, The", likeOther, "").games == 0);
    GameReport op = r; op.opening.known = true; op.opening.eco = "D37"; op.opening.name = "Queen's Gambit Declined, Three Knights";
    op.opening.transposed = true; op.whiteStats.bookMoves = 6;
    CHECK(recordHistory(csv, historyRows(op, "op1", "2026-11-19 18:00"), err));
    std::vector<HistoryRow> withOp = readHistory(csv);
    CHECK(withOp.back().opening == "Queen's Gambit Declined, Three Knights" && withOp.back().transposed &&
          !withOp.back().outOfBookGap && withOp[withOp.size() - 2].bookMoves == 6);
    std::remove(ini.c_str()); std::remove(csv.c_str());
}


static std::vector<std::string> toks(const char *s) {
    std::vector<std::string> v; std::string t; for (const char *p = s;; ++p) { if (*p == ' ' || !*p) { if (!t.empty()) v.push_back(t); t.clear(); if (!*p) break; } else t += *p; } return v;
}

static void testOpening(const char *lichessDir) {
    // en passant squares only count when a capture is possible
    CHECK(positionKey("rnbqkbnr/pppp1ppp/4p3/8/2PP4/8/PP2PPPP/RNBQKBNR b KQkq d3 0 2") ==
          positionKey("rnbqkbnr/pppp1ppp/4p3/8/2PP4/8/PP2PPPP/RNBQKBNR b KQkq c3 0 2"));
    CHECK(positionKey("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3") !=
          positionKey("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3"));

    // Small hand-made book: gap + transposition
    OpeningBook small;
    CHECK(small.addNamedLine("D37", "Test QGD", "1. d4 d5 2. c4 e6 3. Nc3 Nf6"));
    OpeningInfo g = classifyOpening(small, "", toks("d4 Nf6 c4 e6 Nc3 d5 Bg5"));
    CHECK(g.known && g.name == "Test QGD" && g.transposed && g.gap);
    CHECK(g.bookMove[0] && !g.bookMove[1] && !g.bookMove[4] && g.bookMove[5] && !g.bookMove[6]);
    CHECK(g.gapFirstPly == 2 && g.gapLastPly == 5 && g.reentryPly == 6 && g.leftBookPly == 7);
    CHECK(g.label() == "D37 Test QGD (T) (O)");
    CHECK(gapTsvLine(g, toks("d4 Nf6 c4 e6 Nc3 d5 Bg5")) == "D37\tTest QGD\t1. d4 Nf6 2. c4 e6 3. Nc3 d5");
    OpeningInfo straight = classifyOpening(small, "", toks("d4 d5 c4 e6 Nc3 Nf6"));
    CHECK(straight.known && !straight.transposed && !straight.gap && straight.leftBookPly == 0);

    // Real names from Lichess's list
    OpeningBook book;
    std::string err;
    int lines = 0;
    for (const char *f : {"a", "b", "c", "d", "e"})
        lines += book.addTsvFile(std::string(lichessDir) + "/" + f + ".tsv", err);
    CHECK(lines > 3500);
    CHECK(book.namedLines() > 3000 && book.positions() > 7000);
    OpeningInfo ruy = classifyOpening(book, "", toks("e4 e5 Nf3 Nc6 Bb5"));
    CHECK(ruy.known && ruy.eco == "C60" && ruy.name.find("Ruy Lopez") == 0 && !ruy.transposed && !ruy.gap);
    OpeningInfo eng = classifyOpening(book, "", toks("c4 e6 Nc3 d5 d4 Nf6"));
    CHECK(eng.known && eng.name.find("Queen's Gambit Declined") == 0 && eng.transposed);
    CHECK(eng.viaName.find("English") == 0);
    std::printf("  e.g. %s  |  %s\n", eng.label().c_str(), eng.details(toks("1.c4 1...e6 2.Nc3 2...d5 3.d4 3...Nf6")).c_str());
    OpeningInfo none = classifyOpening(book, "8/8/8/4k3/8/8/8/6QK w - - 0 1", toks("Qg5+"));
    CHECK(!none.known && none.bookPlies == 0);

    // Save / load
    book.version = "test";
    std::vector<std::pair<uint64_t, uint32_t>> cs = {{positionKey("8/8/8/8/8/8/8/K6k w - - 0 1"), 7}};
    book.setCounts(cs);
    CHECK(book.count(cs[0].first) == 7);
    CHECK(book.save("wolf_test_book.bin", err));
    OpeningBook back;
    CHECK(back.load("wolf_test_book.bin", err) && back.version == "test");
    CHECK(back.positions() == book.positions() && back.namedLines() == book.namedLines());
    CHECK(back.count(cs[0].first) == 7);
    OpeningInfo eng2 = classifyOpening(back, "", toks("c4 e6 Nc3 d5 d4 Nf6"));
    CHECK(eng2.label() == eng.label());
    std::remove("wolf_test_book.bin");
}

static void testBookScoring() {
    // 1.e4 (book) e5 (book) 2.Qh5 (not book, loses a lot) : book moves aren't judged or counted
    GameReport r;
    r.evals = {Score::Cp(20), Score::Cp(30), Score::Cp(25), Score::Cp(-300)};
    for (int i = 0; i < 3; ++i) {
        MoveReport m; m.ply = i + 1; m.white = i % 2 == 0; m.moveNumber = i / 2 + 1;
        m.before = r.evals[i]; m.after = r.evals[i + 1]; m.book = i < 2;
        r.moves.push_back(m);
    }
    r.moves[1].before = Score::Cp(30); r.moves[1].after = Score::Cp(900);   // a "bad" book move: still not punished
    scoreGame(r, true);
    CHECK(r.moves[1].judgement == Judgement::None);
    CHECK(r.whiteStats.bookMoves == 1 && r.blackStats.bookMoves == 1);
    CHECK(r.whiteStats.moves == 1 && r.blackStats.moves == 0 && !r.blackStats.hasAccuracy);
    CHECK(r.moves[2].judgement == Judgement::Blunder && r.whiteStats.blunders == 1);
    CHECK(formatReport(r).find("Book") != std::string::npos);
}

static void testMyBook() {
    std::string ini = "wolf_test_mb.ini";
    std::remove(myBookPath(ini).c_str()); std::remove(bookGapsPath(ini).c_str()); std::remove("WolfBook.bin");
    ReviewSettings rs; OpeningBook b; std::string st, err;
    CHECK(!loadReviewBook(ini, rs, {}, b, st));                  // nothing there yet
    OpeningBook small; small.addNamedLine("D37", "Test QGD", "1. d4 d5 2. c4 e6 3. Nc3 Nf6");
    std::vector<std::string> g = toks("d4 Nf6 c4 e6 Nc3 d5 Bg5");
    OpeningInfo info = classifyOpening(small, "", g);
    CHECK(info.gap);
    CHECK(addToMyBook(ini, gapTsvLine(info, g), err));
    CHECK(loadReviewBook(ini, rs, {}, b, st) && st.find("1 line") != std::string::npos);
    OpeningInfo again = classifyOpening(b, "", g);
    CHECK(!again.gap && again.bookMove[1] && again.name == "Test QGD");
    small.version = "unit"; CHECK(small.save("WolfBook.bin", err));
    CHECK(loadReviewBook(ini, rs, {}, b, st) && st.find("unit") != std::string::npos);
    GameReport r; r.white = "A"; r.black = "B"; r.opening = info;
    CHECK(logBookGap(ini, r, "g1", gapTsvLine(info, g), err) && logBookGap(ini, r, "g1", gapTsvLine(info, g), err));
    std::string text; readFile(bookGapsPath(ini), text);
    CHECK(text.find("g1\t") != std::string::npos && text.find("g1\t") == text.rfind("g1\t"));   // logged once
    std::remove(myBookPath(ini).c_str()); std::remove(bookGapsPath(ini).c_str()); std::remove("WolfBook.bin");
}

static void testGapRule() {
    OpeningBook small;
    small.addNamedLine("D00", "Test QP", "1. d4 d5");
    small.addNamedLine("D37", "Test QGD", "1. d4 d5 2. c4 e6 3. Nc3 Nf6");
    std::vector<std::string> g = toks("d4 Nf6 c4 e6 Nc3 d5 Bg5");
    auto make = [&](int gapLossCp) {
        GameReport r;
        r.opening = classifyOpening(small, "", g);
        for (size_t i = 0; i < g.size(); ++i) {
            MoveReport m; m.ply = int(i) + 1; m.white = i % 2 == 0; m.moveNumber = int(i) / 2 + 1; m.san = g[i];
            m.before = Score::Cp(20); m.after = Score::Cp(20);
            m.book = r.opening.bookMove[i];
            r.moves.push_back(m);
        }
        r.moves[2].after = Score::Cp(20 - gapLossCp);   // 2.c4 (out of book) loses this much for White
        r.evals.assign(g.size() + 1, Score::Cp(20));
        return r;
    };
    GameReport ok = make(10);                    // a fine move: the (O) re-entry stands
    CHECK(!applyGapRule(ok, small, "", g) && ok.opening.gap && ok.moves[5].book && ok.opening.name == "Test QGD");
    GameReport bad = make(250);                  // a mistake out of book: re-entry not counted
    CHECK(applyGapRule(bad, small, "", g));
    CHECK(bad.opening.gapRejected && !bad.opening.gap && bad.opening.bookEndPly == 1);
    CHECK(bad.moves[0].book && !bad.moves[5].book);     // 3...d5 is judged now
    CHECK(bad.opening.droppedName == "Test QGD" && bad.opening.leftBookPly == 2);
    std::printf("  e.g. %s  |  %s\n", bad.opening.label().c_str(),
                bad.opening.details(toks("1.d4 1...Nf6 2.c4 2...e6 3.Nc3 3...d5 4.Bg5")).c_str());
    CHECK(bad.opening.label().find("book ends at 1. d4)") != std::string::npos);
    scoreGame(bad, true);
    CHECK(bad.whiteStats.bookMoves == 1 && bad.blackStats.bookMoves == 0);
    std::vector<HistoryRow> hr = historyRows(bad, "x", "now");
    CHECK(!hr.empty() && hr[0].gapNotCounted && hr[0].bookEndPly == 1);
}

// Wulfy's sample game 1 (tests/games/wulfy-sample-1.pgn), judged by Stockfish 11 at depth 20.
// Each row is what analysis.cpp hands judgeSpecial (FEN|move|line|best|played|hasSecond|second|
// isBest|prevMove|prevWasCapture|book). Wulfy's verdicts: 15...Nc3+, 23.Nc4, 30.Rd5 and 31.Qxc8+ are
// brilliant; 11.Bd3 is just best; and there were too many Greats.
static Score parseScore(const std::string &s) { return s[0] == 'M' ? Score::Mate(std::stoi(s.substr(1))) : Score::Cp(std::stoi(s.substr(1))); }

static SpecialInput specialRow(const std::string &row)
{
    std::vector<std::string> c;
    size_t a = 0;
    for (size_t b; (b = row.find('|', a)) != std::string::npos; a = b + 1) c.push_back(row.substr(a, b - a));
    c.push_back(row.substr(a));
    SpecialInput in;
    in.fenBefore = c[0]; in.playedUci = c[1];
    for (size_t x = 0, y; x < c[2].size(); x = y + 1) {
        y = c[2].find(',', x);
        if (y == std::string::npos) y = c[2].size();
        in.line.push_back(c[2].substr(x, y - x));
    }
    in.best = parseScore(c[3]); in.played = parseScore(c[4]); in.hasSecond = c[5] == "1"; in.second = parseScore(c[6]);
    in.isBest = c[7] == "1"; in.prevMoveUci = c[8]; in.prevWasCapture = c[9] == "1"; in.book = c[10] == "1";
    return in;
}

static void testSpecialWulfyGame()
{
    struct Row { const char *move; const char *data; Special want; bool followsUp; };
    const Row rows[] = {
        {"4.Nxe4", "rnbqkbnr/ppp2ppp/4p3/8/3Pp3/2N5/PPP2PPP/R1BQKBNR w KQkq - 0 4|c3e4|c3e4,g8f6,e4f6,g7f6,g1f3,b8c6,f1d3,h8g8,e1g1,e6e5|C108|C108|1|C-50|1|e7e6|0|0", Special::None, false},
        {"6.Bd3", "rnbqk1nr/pp2bppp/2p1p3/8/3PN3/5N2/PPP2PPP/R1BQKB1R w KQkq - 2 6|f1d3|f1d3,g8f6,e1g1,f6e4,d3e4,c6c5,d4c5,d8d1,f1d1,e7c5|C155|C155|1|C154|1|f8e7|0|0", Special::None, false},
        {"11.Bd3", "r1bqk2r/pp2bpp1/2p1pn1p/8/3PB3/4BN2/PPPQ1PPP/2KR3R w kq - 1 11|e4d3|e4d3,b7b5,f3e5,d8c7,d2e2,a7a5,e5c6,e7d6,c6e5,e8g8|C85|C85|1|C0|1|d7f6|0|0", Special::None, false},
        {"13.Qe2", "r1bqk2r/pp3pp1/2p1p2p/3nN3/1b1P4/3BB3/PPPQ1PPP/2KR3R w kq - 5 13|d2e2|d2e2,d5e3,e2e3,d8e7,g2g3,c8d7,c1b1,b4d6,c2c3,e8c8|C136|C136|1|C135|1|e7b4|0|0", Special::None, false},
        {"15...Nc3+", "r1b1k2r/pp3pp1/1qp1p2p/3nN3/1b1P4/3B4/PPP1QPPP/1KBR3R b kq - 10 15|d5c3|d5c3,b2c3,b4c3,d3b5,b6b5,e2b5,c6b5,d1d3,b5b4,a2a3|C-190|C-190|1|C172|1|e3c1|0|0", Special::Brilliant, false},
        {"19.Qxc3", "r1b2rk1/pp3pp1/1q2p2p/1p2N3/3P4/2b2Q2/P1P2PPP/1KBR3R w - - 2 19|f3c3|f3c3,f7f6,e5g6,b5b4,c3h3,f8e8,c1h6,e6e5,h3h5,b6e6|C394|C394|1|C0|1|e8g8|0|0", Special::None, false},
        {"23.Nc4", "r1b2rk1/1p3pp1/3q3p/p2pN3/1p6/4B3/PQP2PPP/1K1R3R w - - 0 23|e5c4|e5c4,d6c6,c4b6,a8a6,b6d5,g8h7,g2g4,c8e6,b2d4,f8c8|C297|C297|1|C284|1|e6d5|1|0", Special::Brilliant, false},
        {"29.Qc5", "5rk1/1p3pp1/2q1b2p/p7/1p1Q4/4B3/P1P2PPP/1K1R3R w - - 2 29|d4c5|d4c5,c6a4,d1d5,f8c8,c5c8,e6c8,d5d8,g8h7,d8c8,a4d7|C446|C430|1|C418|0|e4c6|0|0", Special::None, false},
        {"30.Rd5", "5rk1/1p3pp1/4b2p/p1Q5/qp6/4B3/P1P2PPP/1K1R3R w - - 4 30|d1d5|d1d5,f8c8,c5c8,e6c8,d5d8,g8h7,d8c8,a4d7,c8c5,b7b6|C460|C460|1|C181|1|c6a4|0|0", Special::Brilliant, false},
        {"31.Qxc8+", "2r3k1/1p3pp1/4b2p/p1QR4/qp6/4B3/P1P2PPP/1K5R w - - 6 31|c5c8|c5c8,e6c8,d5d8,g8h7,d8c8,a4d7,c8c5,b7b6,c5c4,d7d5|C457|C457|1|C0|1|f8c8|0|0", Special::Brilliant, true},
        {"32.Rd8+", "2b3k1/1p3pp1/7p/p2R4/qp6/4B3/P1P2PPP/1K5R w - - 0 32|d5d8|d5d8,g8h7,d8c8,a4d7,c8c5,b7b6,c5c4,d7d5,c4g4,f7f5|C445|C445|1|C-280|1|e6c8|1|0", Special::None, true},
        {"33.Rxc8", "2bR4/1p3ppk/7p/p7/qp6/4B3/P1P2PPP/1K5R w - - 2 33|d8c8|d8c8,a4d7,c8c5,b7b6,c5c4,d7d5,c4g4,f7f5,g4d4,d5g2|C481|C481|1|C-402|1|g8h7|0|0", Special::None, true},
        {"54.Ke2", "8/6pk/2q4p/5p2/7P/4BKP1/3R1P2/6R1 w - - 12 54|f3e2|f3e2,c6b5,e2e1,b5b1,d2d1,b1b3,g1h1,f5f4,e3f4,b3f3|C562|C562|1|C-117|1|c4c6|0|0", Special::None, false},
    };
    int brilliant = 0, great = 0;
    for (const Row &r : rows) {
        SpecialInput in = specialRow(r.data);
        in.followsUp = r.followsUp;
        SpecialResult res = judgeSpecial(in);
        if (res.kind != r.want) std::printf("  %s: got %s (%s), want %s\n", r.move, specialName(res.kind), res.why.c_str(), specialName(r.want));
        CHECK(res.kind == r.want);
        brilliant += res.kind == Special::Brilliant;
        great += res.kind == Special::Great;
    }
    CHECK(brilliant == 4 && great == 0);

    // 30.Rd5's own sacrifice is the rook left hanging to ...Bxd5, not the queen given up a move later
    SpecialResult rd5 = judgeSpecial(specialRow(rows[8].data));
    CHECK(rd5.why.find("rook") != std::string::npos);
    // 6.Bd3 with its line cut off right after ...Qxd1 (before Rxd1): a cut-off line never counts as
    // "the material stayed lost", and that capture is three moves away anyway
    SpecialInput cut = specialRow(rows[1].data);
    cut.line.resize(8);
    CHECK(judgeSpecial(cut).kind == Special::None);
    // Without the follow-up flag, 32.Rd8+ would be a Great; with it, it's part of 31.Qxc8+'s combination
    SpecialInput rd8 = specialRow(rows[10].data);
    CHECK(judgeSpecial(rd8).kind == Special::Great);
    // 19.Qxc3 is the only good move but it takes a free bishop; capturing a defended piece of less
    // value than the capturer is different and could still be Great
    SpecialInput qxc3 = specialRow(rows[5].data);
    CHECK(judgeSpecial(qxc3).kind == Special::None);
}

static void testSpecial() {
    // Greek gift: 1.Bxh7+! Kxh7 2.Ng5+ Kg8 3.Qh5 - a bishop for a pawn, and White stays clearly better
    const char *greek = "rnbq1rk1/pppn1ppp/4p3/3pP3/1b1P4/2NB1N2/PPP2PPP/R2QK2R w KQ - 0 1";
    SpecialInput g;
    g.fenBefore = greek; g.playedUci = "d3h7"; g.line = {"d3h7", "g8h7", "f3g5", "h7g8", "d1h5"};
    g.best = Score::Cp(250); g.played = Score::Cp(250); g.isBest = true;
    g.hasSecond = true; g.second = Score::Cp(60);
    SpecialResult r = judgeSpecial(g);
    CHECK(r.kind == Special::Brilliant);
    std::printf("  e.g. Bxh7+: %s, %s\n", specialName(r.kind), r.why.c_str());
    CHECK(r.why.find("bishop (2 points down)") != std::string::npos);
    SpecialInput weak = g; weak.played = Score::Cp(40); weak.best = Score::Cp(40);     // sound but only equal
    CHECK(judgeSpecial(weak).kind != Special::Brilliant);
    SpecialInput unsound = g; unsound.played = Score::Cp(-200); unsound.isBest = false; // a bad sacrifice
    CHECK(judgeSpecial(unsound).kind == Special::None);
    SpecialInput book = g; book.book = true;
    CHECK(judgeSpecial(book).kind == Special::None);
    SpecialInput crushing = g; crushing.best = crushing.played = Score::Cp(800); crushing.second = Score::Cp(780);
    CHECK(judgeSpecial(crushing).kind == Special::None);                                 // already winning, not needed

    // A plain trade is not a sacrifice: 4.Bxc6 dxc6 5.O-O
    SpecialInput trade;
    trade.fenBefore = "r1bqkbnr/1ppp1ppp/p1n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 4";
    trade.playedUci = "b5c6"; trade.line = {"b5c6", "d7c6", "e1g1"};
    trade.best = trade.played = Score::Cp(150); trade.isBest = true;
    CHECK(judgeSpecial(trade).kind != Special::Brilliant);
    // Material won straight back is a tactic: 3.Nxe5 Nxe5 4.d4 Ng6?? 5.dxe5 style - here the
    // knight is regained on White's second move after it's taken
    SpecialInput back;
    back.fenBefore = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3";
    back.playedUci = "f3e5"; back.line = {"f3e5", "c6e5", "d2d4", "e5c6", "d4d5", "c6e7", "d5d6"};
    back.best = back.played = Score::Cp(150); back.isBest = true;
    CHECK(judgeSpecial(back).kind == Special::Brilliant);   // stays a piece down for 2+ moves: counts
    back.line = {"f3e5", "c6e5", "d2d4", "e5c6", "d4d5", "c6e5", "f2f4"};   // regained? no: still down -> counts
    CHECK(judgeSpecial(back).kind == Special::Brilliant);
    // Won straight back: 1.Bxf7+ Kxf7 2.Ng5+ Kg8 3.Qxd8 (bishop for the queen two moves later)
    SpecialInput quick;
    quick.fenBefore = "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq - 0 5";
    quick.playedUci = "c4f7"; quick.line = {"c4f7", "e8f7", "f3g5", "f7g8", "d1h5"};
    quick.best = quick.played = Score::Cp(150); quick.isBest = true;
    CHECK(judgeSpecial(quick).kind == Special::Brilliant);    // stays a piece down: a real sacrifice
    quick.line = {"c4f7", "e8f7", "f3e5", "c6e5", "d1h5", "g7g6", "h5e5"};
    CHECK(judgeSpecial(quick).kind != Special::Brilliant);    // more material back within two moves

    // Offered queen that can't be taken: 1.Qg6!! (fxg6 or hxg6 allows Rh8#-style mate) - modelled as
    // the queen hanging to a pawn while the engine's reply declines it
    SpecialInput offer;
    offer.fenBefore = "5rk1/5pp1/7p/8/8/8/5PPP/3Q2KR w - - 0 1";
    offer.playedUci = "d1d5"; offer.line = {"d1d5", "g8h7"};
    offer.best = offer.played = Score::Cp(120); offer.isBest = true;
    CHECK(judgeSpecial(offer).kind != Special::Brilliant);       // queen not attacked: no sacrifice
    offer.fenBefore = "5rk1/5pp1/7p/8/8/8/5PPP/3Q2KR w - - 0 1";
    offer.playedUci = "d1g4"; offer.line = {"d1g4", "g8h7"};   // nothing attacks g4 either
    CHECK(judgeSpecial(offer).kind != Special::Brilliant);
    offer.playedUci = "d1d6"; offer.line = {"d1d6", "g8h7"};   // nothing attacks d6
    CHECK(judgeSpecial(offer).kind != Special::Brilliant);
    offer.fenBefore = "6k1/5pp1/7p/8/8/8/5PPP/3Q2KR w - - 0 1";
    offer.playedUci = "d1g4"; offer.line = {"d1g4"};
    CHECK(judgeSpecial(offer).kind != Special::Brilliant);
    offer.fenBefore = "6k1/5p2/7p/8/8/8/5PPP/3Q2KR w - - 0 1";
    offer.playedUci = "d1g4"; offer.line = {"d1g4", "g8f8"};
    CHECK(judgeSpecial(offer).kind != Special::Brilliant);
    offer.fenBefore = "6k1/5pp1/5n1p/8/8/8/5PPP/3Q2KR w - - 0 1";   // knight f6 attacks g4
    offer.playedUci = "d1g4"; offer.line = {"d1g4", "g8h7"};          // ...and the engine declines it
    SpecialResult off = judgeSpecial(offer);
    CHECK(off.kind == Special::Brilliant && off.why.find("queen") != std::string::npos);

    // Great: the only good move
    SpecialInput only;
    only.fenBefore = "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3";
    only.playedUci = "f1b5"; only.line = {"f1b5"};
    only.best = only.played = Score::Cp(120); only.isBest = true;
    only.hasSecond = true; only.second = Score::Cp(-120);
    SpecialResult gr = judgeSpecial(only);
    CHECK(gr.kind == Special::Great);
    std::printf("  e.g. %s: %s\n", specialName(gr.kind), gr.why.c_str());
    SpecialInput easy = only; easy.second = Score::Cp(90);              // other moves are fine too
    CHECK(judgeSpecial(easy).kind == Special::None);
    SpecialInput small = only; small.best = small.played = Score::Cp(30); small.second = Score::Cp(-80);
    CHECK(judgeSpecial(small).kind == Special::None);                    // doesn't keep +0.8, doesn't save a loss
    SpecialInput save = only; save.best = save.played = Score::Cp(-20); save.second = Score::Cp(-400);
    CHECK(judgeSpecial(save).kind == Special::Great);                    // saves a lost game
    SpecialInput recap = only; recap.prevMoveUci = "c6b5"; recap.prevWasCapture = true;
    CHECK(judgeSpecial(recap).kind == Special::None);                    // just taking back
    SpecialInput notBest = only; notBest.isBest = false;
    CHECK(judgeSpecial(notBest).kind == Special::None);
    SpecialInput cosy = only; cosy.best = cosy.played = Score::Cp(830); cosy.second = Score::Cp(410);
    CHECK(judgeSpecial(cosy).kind == Special::None);                     // next best still +4: not critical

    // Opera Game 16.Qb8+!! Nxb8 17.Rd8#: already crushing, but sacrificing the queen for mate counts
    SpecialInput qb8;
    qb8.fenBefore = "1n2kb1r/p3qppp/4b3/4p1B1/4P3/1Q6/PPP2PPP/3R2K1 w k - 0 16";
    qb8.playedUci = "b3b8"; qb8.line = {"b3b8", "b8b8", "d1d8"};
    qb8.line = {"b3b8", "d7b8", "d1d8"};
    qb8.fenBefore = "4kb1r/p2n1ppp/4q3/4p1B1/4P3/1Q6/PPP2PPP/2KR4 w k - 0 16";
    qb8.best = qb8.played = Score::Mate(2); qb8.isBest = true;
    qb8.hasSecond = true; qb8.second = Score::Cp(950);
    SpecialResult q = judgeSpecial(qb8);
    CHECK(q.kind == Special::Brilliant && q.why.find("forces mate") != std::string::npos);
    std::printf("  e.g. Qb8+: %s, %s\n", specialName(q.kind), q.why.c_str());
    // 17.Rd8# itself is not "great"
    SpecialInput rd8;
    rd8.fenBefore = "1n2kb1r/p4ppp/4q3/4p1B1/4P3/8/PPP2PPP/2KR4 w k - 0 17";
    rd8.playedUci = "d1d8"; rd8.line = {"d1d8"};
    rd8.best = rd8.played = Score::Mate(1); rd8.isBest = true; rd8.hasSecond = true; rd8.second = Score::Cp(-880);
    CHECK(judgeSpecial(rd8).kind == Special::None);
}

int main(int argc, char **argv) {
    if (argc > 2 && std::strcmp(argv[1], "--game-accuracy") == 0) {
        bool whiteStarts = argv[2][0] == 'W';
        std::vector<int> cps;
        for (int i = 3; i < argc; ++i) cps.push_back(std::atoi(argv[i]));
        GameAccuracy g = gameAccuracy(cps, whiteStarts);
        std::printf("%s %s\n", g.hasWhite ? std::to_string(g.white).c_str() : "none",
                                g.hasBlack ? std::to_string(g.black).c_str() : "none");
        return 0;
    }
    testWinPercent(); testMoveAccuracy(); testJudge(); testGameAccuracy(); testPgn(); testScoreGame(); testElo(); testHistory(); testBookScoring(); testMyBook(); testGapRule(); testSpecial(); testSpecialWulfyGame();
    testOpening(argc > 1 ? argv[1] : WOLF_LICHESS_DIR);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
