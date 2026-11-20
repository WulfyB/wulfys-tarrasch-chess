// Runs an engine over a game and produces the per-move and per-player report.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "accuracy.h"
#include "pgn.h"
#include "uci_engine.h"
#include "opening.h"
#include "special.h"

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
    bool        book = false;     // known theory: not judged, not counted in accuracy / loss
    uint32_t    bookGames = 0;    // how many book games reached the position after it
    Special     special = Special::None;   // great (!) / brilliant (!!), see special.h
    std::string specialWhy;
};

struct PlayerStats {
    bool   hasAccuracy = false;
    double accuracy = 0;          // Lichess-style game accuracy
    double acpl = 0;              // average centipawn loss
    int    moves = 0, best = 0, inaccuracies = 0, mistakes = 0, blunders = 0;   // judged (non-book) moves
    int    bookMoves = 0;
    int    brilliant = 0, great = 0;
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
    OpeningInfo opening;          // filled when an opening book was given
    std::string bookVersion;
};

struct AnalysisOptions {
    Method method = Method::SameRoot;
    int depth = 18;
    int movetimeMs = 0;            // if > 0, used instead of depth
    // Called after each position: (done, total). Return false to cancel.
    std::function<bool(int, int)> progress;
    // Optional opening book: book moves are marked and not punished.
    const OpeningBook *book = nullptr;
    // Look for great (!) and brilliant (!!) moves. Needs the engine's top two moves at every
    // position (MultiPV 2), which makes the review roughly 1.3-1.6x slower.
    bool findSpecial = true;
};

GameReport analyseGame(const PgnGame &game, UciEngine &engine, const AnalysisOptions &opt);

// Pure post-processing (no engine): fills cpl/accuracy/judgement/stats from each move's
// before/after scores, and game accuracy using evals for the volatility weights.
// Exposed so it can be unit-tested and reused by the Tarrasch integration.
void scoreGame(GameReport &rep, bool whiteStarts);

// The (O) rule: if the game came back into book only after an out-of-book move that was an
// inaccuracy or worse, the re-entry doesn't count. The opening becomes what it was before the
// gap, book ends at the last move before it, and every move after that is judged normally.
// Needs each move's before/after scores; call before scoreGame(). Returns true if it applied.
bool applyGapRule(GameReport &rep, const OpeningBook &book, const std::string &startFen,
                  const std::vector<std::string> &sanMoves);

} // namespace wolf
