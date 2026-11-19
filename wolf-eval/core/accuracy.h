// Wolf Eval - move and game accuracy, following Lichess's published method.
//
// Sources (checked against the real code, Nov 2026):
//   lichess-org/scalachess  core/src/main/scala/eval.scala      (WinPercent, Cp.CEILING)
//   lichess-org/lila        modules/analyse/.../AccuracyPercent.scala (move + game accuracy)
//   lichess-org/lila        modules/tree/.../Advice.scala        (inaccuracy/mistake/blunder)
//   lichess-org/scalalib    Maths.scala                          (means, standard deviation)
//
// All centipawn values here are from WHITE's point of view unless a name says otherwise.
#pragma once
#include <string>
#include <vector>

namespace wolf {

constexpr int CP_CEILING = 1000;   // evaluations are capped at +/-10 pawns

// An engine score from White's point of view.
struct Score {
    bool is_mate = false;
    int  value   = 0;       // centipawns, or mate distance (+N: White mates in N, -N: Black mates in N)

    static Score Cp(int cp)   { return {false, cp}; }
    static Score Mate(int n)  { return {true, n}; }

    // Capped centipawns: mates count as +/-CP_CEILING.
    int ceiledCp() const;
    // Text like "+0.35", "-1.20", "#3", "#-2".
    std::string str() const;
};

// Winning chances in [-1, +1] and win percent in [0, 100] for a (White POV) centipawn value.
double winningChances(int cp);
double winPercent(int cp);

// Accuracy (0..100) of one move, given the mover's win percent before and after it.
double moveAccuracy(double moverWinBefore, double moverWinAfter);

enum class Judgement { None, Inaccuracy, Mistake, Blunder };
const char *judgementName(Judgement j);

// Lichess's thresholds: winning-chances drop of >= 0.1 / 0.2 / 0.3 for the mover.
Judgement judge(int cpBeforeWhite, int cpAfterWhite, bool moverIsWhite);

struct GameAccuracy {
    bool   hasWhite = false, hasBlack = false;
    double white = 0, black = 0;
};

// cps: evaluation of every position in the game INCLUDING the start position, White POV.
// whiteStarts: true for a normal game (false if the PGN starts from a FEN with Black to move).
// Mirrors lila's AccuracyPercent.gameAccuracy: mean of a volatility-weighted mean and a
// harmonic mean of the per-move accuracies.
GameAccuracy gameAccuracy(const std::vector<int> &cps, bool whiteStarts);

// Same weighting, but with per-move accuracies supplied by the caller (one per move, in order).
// Used by the "same root" method, where each move is judged against the best move from the
// same position instead of against the next position's evaluation.
GameAccuracy gameAccuracy(const std::vector<int> &cps, bool whiteStarts, const std::vector<double> &perMoveAccuracy);

// Per-move accuracies in game order (one per move), using the same per-move formula.
std::vector<double> moveAccuracies(const std::vector<int> &cps, bool whiteStarts);

} // namespace wolf
