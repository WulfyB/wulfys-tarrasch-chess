#include <cstdint>
#include <string>
#include "accuracy.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wolf {

int Score::ceiledCp() const {
    if (is_mate) return value >= 0 ? CP_CEILING : -CP_CEILING;   // "mate 0" = side to move is mated; caller sets sign
    return std::clamp(value, -CP_CEILING, CP_CEILING);
}

std::string Score::str() const {
    char buf[32];
    if (is_mate) std::snprintf(buf, sizeof buf, "#%d", value);
    else         std::snprintf(buf, sizeof buf, "%+.2f", value / 100.0);
    return buf;
}

double winningChances(int cp) {
    cp = std::clamp(cp, -CP_CEILING, CP_CEILING);
    const double MULTIPLIER = -0.00368208;            // lila PR #11148
    double w = 2.0 / (1.0 + std::exp(MULTIPLIER * cp)) - 1.0;
    return std::clamp(w, -1.0, 1.0);
}

double winPercent(int cp) { return 50.0 + 50.0 * winningChances(cp); }

double moveAccuracy(double before, double after) {
    if (after >= before) return 100.0;
    double winDiff = before - after;
    double raw = 103.1668100711649 * std::exp(-0.04354415386753951 * winDiff) - 3.166924740191411;
    raw += 1.0;                                       // lila's "uncertainty bonus"
    return std::clamp(raw, 0.0, 100.0);
}

const char *judgementName(Judgement j) {
    switch (j) {
        case Judgement::Inaccuracy: return "Inaccuracy";
        case Judgement::Mistake:    return "Mistake";
        case Judgement::Blunder:    return "Blunder";
        default:                    return "";
    }
}

Judgement judge(int cpBefore, int cpAfter, bool moverIsWhite) {
    double d = winningChances(cpAfter) - winningChances(cpBefore);
    double delta = moverIsWhite ? -d : d;             // positive = mover lost winning chances
    if (delta >= 0.3) return Judgement::Blunder;
    if (delta >= 0.2) return Judgement::Mistake;
    if (delta >= 0.1) return Judgement::Inaccuracy;
    return Judgement::None;
}

static double stdDev(const std::vector<double> &a) {
    double mean = 0;
    for (double x : a) mean += x;
    mean /= a.size();
    double s = 0;
    for (double x : a) s += (x - mean) * (x - mean);
    return std::sqrt(s / a.size());                   // population SD, as scalalib
}

// Accuracy of each move, aligned to plies; flag says whether the mover is White.
static void perMove(const std::vector<int> &cps, bool whiteStarts,
                    std::vector<double> &acc, std::vector<bool> &moverWhite) {
    acc.clear(); moverWhite.clear();
    for (size_t i = 0; i + 1 < cps.size(); ++i) {
        bool white = ((i % 2 == 0) == whiteStarts);
        double p = winPercent(cps[i]), n = winPercent(cps[i + 1]);
        acc.push_back(white ? moveAccuracy(p, n) : moveAccuracy(100 - p, 100 - n));
        moverWhite.push_back(white);
    }
}

std::vector<double> moveAccuracies(const std::vector<int> &cps, bool whiteStarts) {
    std::vector<double> acc; std::vector<bool> w;
    perMove(cps, whiteStarts, acc, w);
    return acc;
}

GameAccuracy gameAccuracy(const std::vector<int> &cps, bool whiteStarts) {
    return gameAccuracy(cps, whiteStarts, moveAccuracies(cps, whiteStarts));
}

GameAccuracy gameAccuracy(const std::vector<int> &cps, bool whiteStarts, const std::vector<double> &perMoveAccuracy) {
    GameAccuracy r;
    if (cps.size() < 2) return r;

    std::vector<double> wins;
    for (int cp : cps) wins.push_back(winPercent(cp));

    int windowSize = std::clamp(static_cast<int>((cps.size() - 1) / 10), 2, 8);
    int ws = std::min<int>(windowSize, wins.size());

    // Windows: (windowSize - 2) copies of the first window, then every sliding window.
    std::vector<std::vector<double>> windows;
    std::vector<double> first(wins.begin(), wins.begin() + ws);
    for (int k = 0; k < ws - 2; ++k) windows.push_back(first);
    for (size_t s = 0; s + windowSize <= wins.size(); ++s)
        windows.emplace_back(wins.begin() + s, wins.begin() + s + windowSize);
    if (wins.size() < static_cast<size_t>(windowSize)) windows.push_back(wins);  // Scala's sliding() on a short list

    std::vector<double> acc; std::vector<bool> moverWhite;
    perMove(cps, whiteStarts, acc, moverWhite);
    for (size_t i = 0; i < acc.size() && i < perMoveAccuracy.size(); ++i) acc[i] = perMoveAccuracy[i];

    for (int side = 0; side < 2; ++side) {
        bool white = side == 0;
        double wsum = 0, wtot = 0, hsum = 0; int n = 0;
        for (size_t i = 0; i < acc.size() && i < windows.size(); ++i) {   // zip() stops at the shorter list
            if (moverWhite[i] != white) continue;
            double w = std::clamp(stdDev(windows[i]), 0.5, 12.0);
            wsum += acc[i] * w; wtot += w;
            hsum += 1.0 / std::max(1.0, acc[i]);
            ++n;
        }
        if (n == 0 || wtot == 0) continue;
        double weighted = wsum / wtot, harmonic = n / hsum;
        double v = (weighted + harmonic) / 2;
        if (white) { r.hasWhite = true; r.white = v; }
        else       { r.hasBlack = true; r.black = v; }
    }
    return r;
}

} // namespace wolf
