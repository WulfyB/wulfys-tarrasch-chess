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
    testWinPercent(); testMoveAccuracy(); testJudge(); testGameAccuracy(); testPgn(); testScoreGame(); testElo();
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
