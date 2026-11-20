#include "special.h"
#include <cctype>
#include <cstdio>
#include "thc.h"

namespace wolf {

const char *specialName(Special s)   { return s == Special::Brilliant ? "Brilliant" : s == Special::Great ? "Great" : ""; }
const char *specialSymbol(Special s) { return s == Special::Brilliant ? "!!" : s == Special::Great ? "!" : ""; }

namespace {

int pieceValue(char p)
{
    switch (std::toupper((unsigned char)p)) {
        case 'P': return 1;
        case 'N': case 'B': return 3;
        case 'R': return 5;
        case 'Q': return 9;
        default:  return 0;
    }
}

const char *pieceName(char p)
{
    switch (std::toupper((unsigned char)p)) {
        case 'P': return "a pawn";
        case 'N': return "a knight";
        case 'B': return "a bishop";
        case 'R': return "a rook";
        case 'Q': return "the queen";
        default:  return "material";
    }
}

// Material of `white` minus the other side.
int materialDiff(const thc::ChessRules &cr, bool white)
{
    int w = 0, b = 0;
    for (int i = 0; i < 64; ++i) {
        char c = cr.squares[i];
        if (c >= 'A' && c <= 'Z') w += pieceValue(c);
        else if (c >= 'a' && c <= 'z') b += pieceValue(c);
    }
    return white ? w - b : b - w;
}

int moverCp(const Score &s, bool white) { return white ? s.ceiledCp() : -s.ceiledCp(); }

std::string pawns(int cp)
{
    char b[32];
    if (cp >= CP_CEILING) return "mating";
    std::snprintf(b, sizeof b, "%+.1f", cp / 100.0);
    return b;
}

std::string squareOf(const std::string &uci) { return uci.size() >= 4 ? uci.substr(2, 2) : std::string(); }

char pieceAt(const thc::ChessRules &cr, const std::string &sq)
{
    if (sq.size() != 2) return ' ';
    int file = sq[0] - 'a', rank = sq[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return ' ';
    return cr.squares[(7 - rank) * 8 + file];   // thc: a8 = 0, h1 = 63
}

// Can the side to move capture on `sq` and win at least `need` points (with a simple
// "is it defended" check)? Returns the best gain found (0 if none).
int bestCaptureGain(thc::ChessRules cr, const std::string &sq)
{
    thc::MOVELIST list;
    cr.GenLegalMoveList(&list);
    int best = 0;
    char target = pieceAt(cr, sq);
    for (int i = 0; i < list.count; ++i) {
        std::string uci = list.moves[i].TerseOut();
        if (squareOf(uci) != sq) continue;
        char capturer = pieceAt(cr, uci.substr(0, 2));
        thc::ChessRules after = cr;
        after.PlayMove(list.moves[i]);
        // Can the other side take back on that square?
        thc::MOVELIST back;
        after.GenLegalMoveList(&back);
        bool defended = false;
        for (int j = 0; j < back.count; ++j)
            if (squareOf(back.moves[j].TerseOut()) == sq) { defended = true; break; }
        int gain = pieceValue(target) - (defended ? pieceValue(capturer) : 0);
        if (gain > best) best = gain;
    }
    return best;
}

} // namespace

SpecialResult judgeSpecial(const SpecialInput &in)
{
    SpecialResult res;
    if (in.book || in.playedUci.size() < 4)
        return res;
    thc::ChessRules cr;
    if (!cr.Forsyth(in.fenBefore.c_str()))
        return res;
    bool white = cr.WhiteToPlay();
    thc::Move mv;
    if (!mv.TerseIn(&cr, in.playedUci.c_str()))
        return res;

    int bestCp = moverCp(in.best, white);
    int playedCp = in.isBest ? bestCp : moverCp(in.played, white);
    int loss = bestCp - playedCp;
    if (loss < 0) loss = 0;
    bool onlyMove = false;
    int secondCp = 0;
    if (in.hasSecond) {
        secondCp = moverCp(in.second, white);
        onlyMove = winningChances(bestCp) - winningChances(secondCp) >= kOnlyMoveGap;
    }

    // ---------------- sacrifice?
    int base = materialDiff(cr, white);
    char moved = pieceAt(cr, in.playedUci.substr(0, 2));
    int sacPoints = 0;
    char sacPiece = ' ';
    {
        thc::ChessRules r = cr;
        std::vector<int> deficit;   // after each ply of the line
        std::vector<char> lost;     // piece captured by that ply (if any)
        for (size_t k = 0; k < in.line.size() && k < 8; ++k) {
            thc::Move m;
            if (!m.TerseIn(&r, in.line[k].c_str())) break;
            lost.push_back(pieceAt(r, squareOf(in.line[k])));
            r.PlayMove(m);
            deficit.push_back(base - materialDiff(r, white));
        }
        // After an opponent ply (index 1, 3, 5) the mover is >= 2 down, and stays that far down for
        // the next three plies (two more of the mover's own moves) or until the line ends (mate).
        // Winning the material straight back is a tactic, not a sacrifice.
        for (size_t k = 1; k < deficit.size(); k += 2) {
            bool persists = true;
            for (size_t j = k + 1; j <= k + 3 && j < deficit.size(); ++j)
                if (deficit[j] < kSacrificePoints) persists = false;
            if (deficit[k] >= kSacrificePoints && persists && deficit[k] > sacPoints) {
                sacPoints = deficit[k];
                sacPiece = lost[k];
            }
        }
    }
    if (sacPoints < kSacrificePoints && pieceValue(moved) >= kSacrificePoints) {
        // Offered but not taken: the moved piece hangs and the engine's reply leaves it.
        thc::ChessRules r = cr;
        r.PlayMove(mv);
        std::string to = squareOf(in.playedUci);
        bool replyTakes = in.line.size() >= 2 && squareOf(in.line[1]) == to;
        int gain = bestCaptureGain(r, to);
        if (!replyTakes && gain >= kSacrificePoints) {
            sacPoints = gain;
            sacPiece = moved;
        }
    }

    // ---------------- brilliant
    if (sacPoints >= kSacrificePoints && loss <= kBrilliantMaxLossCp && playedCp >= kBrilliantMinAfterCp &&
        (bestCp < kAlreadyWinningCp || onlyMove)) {
        res.kind = Special::Brilliant;
        res.why = std::string("sacrifices ") + pieceName(sacPiece) + " (" + std::to_string(sacPoints) +
                  (sacPoints == 1 ? " point" : " points") + " down) and stays " + pawns(playedCp);
        return res;
    }

    // ---------------- great
    if (!in.isBest || !onlyMove)
        return res;
    bool keeps = playedCp >= kGreatKeepCp;
    bool saves = secondCp <= kGreatSavesFromCp && playedCp >= kGreatSavedToCp;
    if (!keeps && !saves)
        return res;
    if (in.prevWasCapture && squareOf(in.prevMoveUci) == squareOf(in.playedUci))
        return res;   // just taking back
    thc::MOVELIST list;
    cr.GenLegalMoveList(&list);
    if (list.count < 3)
        return res;   // (almost) forced
    res.kind = Special::Great;
    res.why = std::string("the only good move: ") + (keeps ? "keeps " + pawns(playedCp) : "saves the game (" + pawns(playedCp) + ")") +
              ", the next best is " + pawns(secondCp);
    return res;
}

} // namespace wolf
