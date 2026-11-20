#include "opening.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include "thc.h"

namespace wolf {

namespace {

const char *kStartFen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

std::vector<std::string> splitWs(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string t;
    while (in >> t) out.push_back(t);
    return out;
}

std::string cleanSan(std::string s)
{
    for (char &c : s) if (c == '0') c = 'O';   // 0-0 -> O-O
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' || s.back() == '?'))
        s.pop_back();
    return s;
}

// "1. d4 d5 2.c4" -> {"d4","d5","c4"}; skips move numbers, results and NAGs.
std::vector<std::string> sanTokens(const std::string &pgn)
{
    std::vector<std::string> out;
    for (std::string t : splitWs(pgn)) {
        size_t i = 0;
        while (i < t.size() && std::isdigit((unsigned char)t[i])) ++i;
        if (i > 0 && i < t.size() && t[i] == '.') {
            while (i < t.size() && t[i] == '.') ++i;
            t = t.substr(i);
        } else if (i == t.size()) {
            continue;   // bare number
        }
        if (t.empty() || t[0] == '$' || t == "*" || t == "1-0" || t == "0-1" || t == "1/2-1/2")
            continue;
        out.push_back(cleanSan(t));
    }
    return out;
}

// Keys of the start position and of every position after each move. False if a move is illegal.
bool lineKeys(const std::vector<std::string> &san, std::vector<uint64_t> &keys)
{
    thc::ChessRules cr;
    keys.push_back(positionKey(cr.ForsythPublish()));
    for (const std::string &s : san) {
        thc::Move mv;
        if (!mv.NaturalIn(&cr, s.c_str()))
            return false;
        cr.PlayMove(mv);
        keys.push_back(positionKey(cr.ForsythPublish()));
    }
    return true;
}

void putU32(std::string &b, uint32_t v) { for (int i = 0; i < 4; ++i) b += char((v >> (8 * i)) & 0xFF); }
void putU64(std::string &b, uint64_t v) { for (int i = 0; i < 8; ++i) b += char((v >> (8 * i)) & 0xFF); }
void putStr(std::string &b, const std::string &s) { putU32(b, (uint32_t)s.size()); b += s; }

struct Reader {
    const std::string &b;
    size_t p = 0;
    bool ok = true;
    explicit Reader(const std::string &s) : b(s) {}
    bool need(size_t n) { if (p + n > b.size()) ok = false; return ok; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t((unsigned char)b[p + i]) << (8 * i); p += 4; return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t((unsigned char)b[p + i]) << (8 * i); p += 8; return v; }
    std::string str() { uint32_t n = u32(); if (!need(n)) return std::string(); std::string s = b.substr(p, n); p += n; return s; }
};

const char kMagic[] = "WOLFBK01";

} // namespace

uint64_t positionKey(const std::string &fen)
{
    std::vector<std::string> f = splitWs(fen);
    while (f.size() < 4) f.push_back("-");
    std::string ep = f[3];
    if (ep != "-" && ep.size() == 2) {
        // Keep the ep square only if a pawn of the side to move stands next to the pawn that just moved.
        char board[8][8] = {};   // [rank 0..7][file 0..7]
        int r = 7, c = 0;
        for (char ch : f[0]) {
            if (ch == '/' || ch == '\\') { --r; c = 0; }
            else if (std::isdigit((unsigned char)ch)) c += ch - '0';
            else { if (r >= 0 && r < 8 && c >= 0 && c < 8) board[r][c] = ch; ++c; }
        }
        int file = ep[0] - 'a';
        bool whiteToMove = f[1] == "w";
        int pawnRank = whiteToMove ? 4 : 3;      // the rank the capturing pawn stands on
        char pawn = whiteToMove ? 'P' : 'p';
        bool can = false;
        for (int d = -1; d <= 1; d += 2) {
            int ff = file + d;
            if (ff >= 0 && ff < 8 && board[pawnRank][ff] == pawn) can = true;
        }
        if (!can) ep = "-";
    }
    std::string key = f[0] + " " + f[1] + " " + f[2] + " " + ep;
    for (char &ch : key) if (ch == '\\') ch = '/';
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : key) { h ^= ch; h *= 1099511628211ULL; }
    return h;
}

// ------------------------------------------------------------------ OpeningBook

uint32_t OpeningBook::count(uint64_t key) const
{
    auto it = std::lower_bound(keys.begin(), keys.end(), key);
    if (it == keys.end() || *it != key) return 0;
    return counts[it - keys.begin()];
}

const NamedLine *OpeningBook::named(uint64_t key) const
{
    auto it = std::lower_bound(nameIndex.begin(), nameIndex.end(), std::make_pair(key, uint32_t(0)));
    if (it == nameIndex.end() || it->first != key) return nullptr;
    return &names[it->second];
}

void OpeningBook::insertPositions(const std::vector<uint64_t> &ks)
{
    std::vector<uint64_t> add(ks);
    std::sort(add.begin(), add.end());
    add.erase(std::unique(add.begin(), add.end()), add.end());
    std::vector<uint64_t> nk;
    std::vector<uint32_t> nc;
    nk.reserve(keys.size() + add.size());
    nc.reserve(keys.size() + add.size());
    size_t i = 0, j = 0;
    while (i < keys.size() || j < add.size()) {
        if (j == add.size() || (i < keys.size() && keys[i] < add[j])) { nk.push_back(keys[i]); nc.push_back(counts[i]); ++i; }
        else if (i == keys.size() || add[j] < keys[i]) { nk.push_back(add[j]); nc.push_back(1); ++j; }
        else { nk.push_back(keys[i]); nc.push_back(std::max<uint32_t>(counts[i], 1)); ++i; ++j; }
    }
    keys.swap(nk);
    counts.swap(nc);
}

void OpeningBook::sortNames()
{
    // First line for a position wins (stable sort keeps file order).
    std::stable_sort(nameIndex.begin(), nameIndex.end(),
                     [](const std::pair<uint64_t, uint32_t> &a, const std::pair<uint64_t, uint32_t> &b) { return a.first < b.first; });
    nameIndex.erase(std::unique(nameIndex.begin(), nameIndex.end(),
                                [](const std::pair<uint64_t, uint32_t> &a, const std::pair<uint64_t, uint32_t> &b) { return a.first == b.first; }),
                    nameIndex.end());
}

bool OpeningBook::addNamedLine(const std::string &eco, const std::string &name, const std::string &pgnMoves)
{
    NamedLine nl;
    nl.eco = eco;
    nl.name = name;
    nl.san = sanTokens(pgnMoves);
    std::vector<uint64_t> ks;
    if (nl.san.empty() || !lineKeys(nl.san, ks))
        return false;
    insertPositions(ks);
    nameIndex.push_back(std::make_pair(ks.back(), (uint32_t)names.size()));
    names.push_back(nl);
    sortNames();
    return true;
}

int OpeningBook::addTsvFile(const std::string &path, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "Can't open " + path; return -1; }
    std::string line;
    std::vector<uint64_t> all;
    int n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::string cur;
        for (char c : line) { if (c == '\t') { f.push_back(cur); cur.clear(); } else cur += c; }
        f.push_back(cur);
        if (f.size() < 3 || f[0] == "eco") continue;
        NamedLine nl;
        nl.eco = f[0];
        nl.name = f[1];
        nl.san = sanTokens(f[2]);
        std::vector<uint64_t> ks;
        if (nl.san.empty() || !lineKeys(nl.san, ks)) continue;
        all.insert(all.end(), ks.begin(), ks.end());
        nameIndex.push_back(std::make_pair(ks.back(), (uint32_t)names.size()));
        names.push_back(nl);
        ++n;
    }
    insertPositions(all);
    sortNames();
    return n;
}

void OpeningBook::setCounts(std::vector<std::pair<uint64_t, uint32_t>> cs)
{
    std::sort(cs.begin(), cs.end());
    keys.clear();
    counts.clear();
    for (const auto &kc : cs) {
        if (!keys.empty() && keys.back() == kc.first) { counts.back() += kc.second; continue; }
        keys.push_back(kc.first);
        counts.push_back(kc.second);
    }
    // keep named lines in the book
    std::vector<uint64_t> ks;
    for (const NamedLine &nl : names) lineKeys(nl.san, ks);
    if (!ks.empty()) insertPositions(ks);
}

bool OpeningBook::save(const std::string &path, std::string &err) const
{
    std::string b(kMagic, 8);
    putStr(b, version);
    putU32(b, (uint32_t)names.size());
    for (const NamedLine &nl : names) {
        putStr(b, nl.eco);
        putStr(b, nl.name);
        std::string line;
        for (size_t i = 0; i < nl.san.size(); ++i) line += (i ? " " : "") + nl.san[i];
        putStr(b, line);
    }
    putU32(b, (uint32_t)keys.size());
    for (uint64_t k : keys) putU64(b, k);
    for (uint32_t c : counts) putU32(b, c);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "Can't write " + path; return false; }
    out.write(b.data(), (std::streamsize)b.size());
    if (!out) { err = "Couldn't finish writing " + path; return false; }
    return true;
}

bool OpeningBook::load(const std::string &path, std::string &err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "No opening book at " + path; return false; }
    std::string b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 8 || b.compare(0, 8, kMagic) != 0) { err = path + " isn't a Wolf opening book"; return false; }
    Reader r(b);
    r.p = 8;
    OpeningBook nb;
    nb.version = r.str();
    uint32_t nn = r.u32();
    for (uint32_t i = 0; i < nn && r.ok; ++i) {
        NamedLine nl;
        nl.eco = r.str();
        nl.name = r.str();
        nl.san = splitWs(r.str());
        nb.names.push_back(nl);
    }
    uint32_t np = r.u32();
    if (!r.need(size_t(np) * 12)) { err = path + " is cut short"; return false; }
    nb.keys.resize(np);
    nb.counts.resize(np);
    for (uint32_t i = 0; i < np; ++i) nb.keys[i] = r.u64();
    for (uint32_t i = 0; i < np; ++i) nb.counts[i] = r.u32();
    if (!r.ok) { err = path + " is damaged"; return false; }
    // Rebuild the name index (cheap: a few thousand short lines)
    for (uint32_t i = 0; i < nb.names.size(); ++i) {
        std::vector<uint64_t> ks;
        if (lineKeys(nb.names[i].san, ks))
            nb.nameIndex.push_back(std::make_pair(ks.back(), i));
    }
    nb.sortNames();
    *this = std::move(nb);
    return true;
}

// ------------------------------------------------------------------ classification

OpeningInfo classifyOpening(const OpeningBook &book, const std::string &startFen,
                            const std::vector<std::string> &sanMoves)
{
    OpeningInfo info;
    size_t n = sanMoves.size();
    info.bookMove.assign(n, false);
    info.games.assign(n, 0);
    if (book.empty() || n == 0)
        return info;
    if (!startFen.empty() && positionKey(startFen) != positionKey(kStartFen))
        return info;

    thc::ChessRules cr;
    std::vector<std::string> played;   // cleaned SAN
    const NamedLine *finalLine = nullptr, *viaLine = nullptr;
    bool finalCanonical = false;
    for (size_t i = 0; i < n; ++i) {
        thc::Move mv;
        std::string s = cleanSan(sanMoves[i]);
        if (!mv.NaturalIn(&cr, s.c_str()))
            break;
        played.push_back(cleanSan(mv.NaturalOut(&cr)));
        cr.PlayMove(mv);
        uint64_t k = positionKey(cr.ForsythPublish());
        uint32_t c = book.count(k);
        info.bookMove[i] = c > 0;
        info.games[i] = c;
        if (const NamedLine *nl = book.named(k)) {
            bool canonical = nl->san.size() == played.size() &&
                             std::equal(nl->san.begin(), nl->san.end(), played.begin());
            finalLine = nl;
            finalCanonical = canonical;
            info.namedPly = int(i) + 1;
            if (canonical)
                viaLine = nl;
        }
    }

    for (size_t i = 0; i < n; ++i)
        if (info.bookMove[i]) ++info.bookPlies;

    if (finalLine) {
        info.known = true;
        info.eco = finalLine->eco;
        info.name = finalLine->name;
        info.transposed = !finalCanonical;
        if (info.transposed && viaLine && viaLine != finalLine && viaLine->name != finalLine->name) {
            info.viaEco = viaLine->eco;
            info.viaName = viaLine->name;
        }
    }

    // Gaps: left the book and came back
    int exit = -1, last = -1;
    for (size_t i = 0; i < n; ++i) {
        if (info.bookMove[i]) last = int(i);
        else if (exit < 0) exit = int(i);
    }
    if (exit >= 0 && last > exit) {
        info.gap = true;
        info.gapFirstPly = exit + 1;
        int re = exit;
        while (re < (int)n && !info.bookMove[re]) ++re;
        info.reentryPly = re + 1;
        info.gapLastPly = re;   // ply before the re-entry
    }
    info.leftBookPly = last + 1 < (int)n ? last + 2 : 0;
    return info;
}

std::string OpeningInfo::label() const
{
    std::string s;
    if (known) s = eco + " " + name;
    else if (bookPlies > 0) s = "Book line (no name)";
    else return s;
    if (transposed) s += " (T)";
    if (gapRejected) {
        int n = (bookEndPly + 1) / 2;
        s += " (O, book ends at " + (bookEndPly > 0 ? std::to_string(n) + (bookEndPly % 2 ? ". " : "... ") + bookEndSan
                                                   : std::string("the start")) + ")";
    } else if (gap) s += " (O)";
    return s;
}

std::string OpeningInfo::details(const std::vector<std::string> &moves) const
{
    auto mv = [&](int ply) {
        return ply >= 1 && ply <= (int)moves.size() ? moves[ply - 1] : std::to_string(ply);
    };
    std::string s;
    if (transposed)
        s += viaName.empty() ? "Reached by a different move order (T). "
                             : "Transposed from " + viaEco + " " + viaName + " (T). ";
    if (gapRejected)
        s += "Came back into book with " + mv(droppedReentryPly) + " (" + droppedEco + " " + droppedName +
             "), but only after a weak move out of book, so the opening isn't counted after " +
             (bookEndPly > 0 ? mv(bookEndPly) : std::string("the start")) + ". ";
    else if (gap)
        s += "Out of book from " + mv(gapFirstPly) + (gapLastPly > gapFirstPly ? " to " + mv(gapLastPly) : "") +
             ", back in book with " + mv(reentryPly) + " (O). ";
    if (gapRejected)
        ;   // already said where book ends
    else if (bookPlies == 0)
        s += "No book moves.";
    else if (leftBookPly > 0)
        s += "Left book with " + mv(leftBookPly) + ".";
    else
        s += gap ? "Still in book at the end." : "Every move was in the book.";
    return s;
}

std::string gapTsvLine(const OpeningInfo &info, const std::vector<std::string> &sanMoves)
{
    if (!info.gap || info.reentryPly <= 0 || info.reentryPly > (int)sanMoves.size())
        return std::string();
    std::string pgn;
    for (int p = 1; p <= info.reentryPly; ++p) {
        if (p % 2 == 1) pgn += (p > 1 ? " " : "") + std::to_string((p + 1) / 2) + ". ";
        else pgn += " ";
        pgn += cleanSan(sanMoves[p - 1]);
    }
    std::string eco = info.known ? info.eco : "A00";
    std::string name = info.known ? info.name : "My line";
    return eco + "\t" + name + "\t" + pgn;
}

} // namespace wolf
