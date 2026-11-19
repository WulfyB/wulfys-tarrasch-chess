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
    testWinPercent(); testMoveAccuracy(); testJudge(); testGameAccuracy(); testPgn(); testScoreGame(); testElo(); testHistory(); testBookScoring();
    testOpening(argc > 1 ? argv[1] : WOLF_LICHESS_DIR);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
