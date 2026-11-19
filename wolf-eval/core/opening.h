// Wolf Eval - opening book: which positions are known theory, what the opening is called,
// and how the game got there.
//
// The book is a set of positions (not move sequences), so transpositions are found for free:
//   * "book" positions were reached in enough strong games (TWIC over-the-board games and the
//     Lichess Elite Database), or are on a named line;
//   * named positions come from Lichess's public-domain chess-openings list (ECO + name + moves).
// Everything lives in one file, WolfBook.bin, rebuilt monthly by .github/workflows/opening-book.yml.
// Your own additions ("WolfReview my book.tsv", same format as the Lichess .tsv files) are
// loaded on top.
//
// classifyOpening() names a game's opening and adds two flags:
//   (T) transposition: the named position was reached by a different move order than its
//       textbook line (e.g. 1.c4 e6 2.Nc3 d5 3.d4 = Queen's Gambit Declined (T) from the English)
//   (O) out of book in between: the game left the book and later came back into a book
//       position. Either the players found a transposition, or the book is missing a line;
//       the out-of-book moves are reported so they can be added and the game re-reviewed.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wolf {

// Position key: FNV-1a of "board side castling ep" from a FEN, with the en passant square
// dropped unless a pawn could actually capture there (so 1.d4 e6 2.c4 == 1.c4 e6 2.d4).
uint64_t positionKey(const std::string &fen);

struct NamedLine {
    std::string eco, name;
    std::vector<std::string> san;   // the textbook move order (without move numbers)
};

class OpeningBook {
public:
    std::string version;            // e.g. "2026-12: TWIC 1471-1672, Lichess Elite 2024-12..2026-11, names 3ba6b20"

    // ---- loading
    bool load(const std::string &path, std::string &err);   // WolfBook.bin
    bool save(const std::string &path, std::string &err) const;
    // A Lichess-style TSV (eco<TAB>name<TAB>pgn, header line optional). Every position on each
    // line becomes book; the final position gets the name. Returns lines read (-1 = can't open).
    int  addTsvFile(const std::string &path, std::string &err);
    // One line; moves as PGN text ("1. d4 d5 2. c4"). Returns false if a move is illegal.
    bool addNamedLine(const std::string &eco, const std::string &name, const std::string &pgnMoves);

    // ---- building (used by the wolf-book tool)
    // Replace all counted positions with these (key, count) pairs (any order; merged with named lines).
    void setCounts(std::vector<std::pair<uint64_t, uint32_t>> counts);

    // ---- lookups
    bool contains(uint64_t key) const { return count(key) > 0; }
    uint32_t count(uint64_t key) const;                 // games that reached it (named-only lines count 1)
    const NamedLine *named(uint64_t key) const;
    size_t positions() const { return keys.size(); }
    size_t namedLines() const { return names.size(); }
    bool empty() const { return keys.empty(); }

private:
    std::vector<uint64_t> keys;     // sorted
    std::vector<uint32_t> counts;   // parallel to keys
    std::vector<std::pair<uint64_t, uint32_t>> nameIndex;   // sorted (key, index into names)
    std::vector<NamedLine> names;
    void insertPositions(const std::vector<uint64_t> &ks);
    void sortNames();
};

struct OpeningInfo {
    bool        known = false;      // reached at least one named position
    std::string eco, name;          // deepest named opening reached
    int         namedPly = 0;       // ply after which it was reached
    bool        transposed = false; // (T)
    std::string viaEco, viaName;    // the opening it transposed from (empty if none on the way)
    bool        gap = false;        // (O)
    int         gapFirstPly = 0, gapLastPly = 0;   // the out-of-book plies before re-entering
    int         reentryPly = 0;     // first ply back in book after the gap
    int         bookPlies = 0;      // number of book moves
    int         leftBookPly = 0;    // first ply after the last book move (0 = never left / no moves)
    std::vector<bool> bookMove;     // per ply: the move led to a book position
    std::vector<uint32_t> games;    // per ply: how many book games reached that position

    // "D37 Queen's Gambit Declined: Three Knights Variation (T)(O)"
    std::string label() const;
    // Longer, one line: adds "from ..." and where the game left / re-entered book.
    std::string details(const std::vector<std::string> &sanWithNumbers) const;
};

// startFen empty = the normal start position (games from other positions get no opening).
OpeningInfo classifyOpening(const OpeningBook &book, const std::string &startFen,
                            const std::vector<std::string> &sanMoves);

// For "Add to my book": the moves up to and including the re-entry, as a TSV line in the
// same format as the Lichess files. Empty if the game has no (O) gap.
std::string gapTsvLine(const OpeningInfo &info, const std::vector<std::string> &sanMoves);

} // namespace wolf
