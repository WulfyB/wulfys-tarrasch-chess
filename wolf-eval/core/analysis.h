// Runs an engine over a game and produces the per-move and per-player report.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "accuracy.h"
#include "pgn.h"
#include "uci_engine.h"

namespace wolf {

enum class Method {
    SameRoot,   // default: best move vs your move, both scored from the same position (stable with any engine)
    Lichess     // Lichess's exact method: evaluation before vs evaluation after each move
};

struct MoveReport {
    int         ply = 0;          // 1 = White's first move
    int         moveNumber = 0;
    bool        white = true;     // mover
    std::string san;              // move played
    std::string bestSan;          // engine's choice in the position before the move
    bool        isBest = false;
    Score       before;           // White POV: value of the best move (same-root) or of the position before (lichess)
    Score       after;            // White POV: value of the move played (same-root) or of the next position (lichess)
    int         cpl = 0;          // centipawn loss for the mover (capped evals, >= 0)
    double      accuracy = 100;   // 0..100
    Judgement   judgement = Judgement::None;
};

struct PlayerStats {
    bool   hasAccuracy = false;
    double accuracy = 0;          // Lichess-style game accuracy
    double acpl = 0;              // average centipawn loss
    int    moves = 0, best = 0, inaccuracies = 0, mistakes = 0, blunders = 0;
};

struct GameReport {
    bool        ok = false;
    std::string error;
    std::string white, black, result, event, date, whiteElo, blackElo;
    std::string engineName;
    int         depth = 0, movetimeMs = 0;
    Method      method = Method::SameRoot;
    std::vector<MoveReport> moves;
    std::vector<Score>      evals;   // every position incl. start, White POV
    PlayerStats whiteStats, blackStats;
};

struct AnalysisOptions {
    Method method = Method::SameRoot;
    int depth = 18;
    int movetimeMs = 0;            // if > 0, used instead of depth
    // Called after each position: (done, total). Return false to cancel.
    std::function<bool(int, int)> progress;
};

GameReport analyseGame(const PgnGame &game, UciEngine &engine, const AnalysisOptions &opt);

// Pure post-processing (no engine): fills cpl/accuracy/judgement/stats from each move's
// before/after scores, and game accuracy using evals for the volatility weights.
// Exposed so it can be unit-tested and reused by the Tarrasch integration.
void scoreGame(GameReport &rep, bool whiteStarts);

} // namespace wolf
