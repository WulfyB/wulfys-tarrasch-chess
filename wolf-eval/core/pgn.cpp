#include "pgn.h"
#include <cctype>
#include <fstream>
#include <sstream>

namespace wolf {

bool readFile(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);                                  // UTF-8 BOM
    return true;
}

static bool isResult(const std::string &t) {
    return t == "1-0" || t == "0-1" || t == "1/2-1/2" || t == "*";
}

static std::string stripAnnotations(std::string t) {
    while (!t.empty() && (t.back() == '!' || t.back() == '?')) t.pop_back();
    return t;
}

std::vector<PgnGame> parsePgn(const std::string &text) {
    std::vector<PgnGame> games;
    PgnGame cur;
    bool inMoves = false;
    size_t i = 0, n = text.size();

    auto finish = [&]() {
        if (!cur.headers.empty() || !cur.sanMoves.empty()) games.push_back(cur);
        cur = PgnGame();
        inMoves = false;
    };

    while (i < n) {
        char c = text[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }

        if (c == '[' && !inMoves) {                       // header: [Key "Value"]
            size_t end = text.find(']', i);
            if (end == std::string::npos) break;
            std::string line = text.substr(i + 1, end - i - 1);
            size_t sp = line.find(' ');
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            if (sp != std::string::npos && q1 != std::string::npos && q2 > q1)
                cur.headers[line.substr(0, sp)] = line.substr(q1 + 1, q2 - q1 - 1);
            i = end + 1;
            continue;
        }
        if (c == '[' && inMoves) { finish(); continue; } // next game started without a result token

        inMoves = true;
        if (c == '{') {                                   // comment
            size_t end = text.find('}', i);
            i = (end == std::string::npos) ? n : end + 1;
            continue;
        }
        if (c == ';') {                                   // rest-of-line comment
            size_t end = text.find('\n', i);
            i = (end == std::string::npos) ? n : end + 1;
            continue;
        }
        if (c == '(') {                                   // variation (may nest)
            int depth = 0;
            for (; i < n; ++i) {
                if (text[i] == '{') { size_t e = text.find('}', i); i = (e == std::string::npos) ? n - 1 : e; continue; }
                if (text[i] == '(') ++depth;
                else if (text[i] == ')' && --depth == 0) { ++i; break; }
            }
            continue;
        }
        if (c == '$') {                                   // NAG
            ++i;
            while (i < n && std::isdigit((unsigned char)text[i])) ++i;
            continue;
        }

        size_t start = i;
        while (i < n && !std::isspace((unsigned char)text[i]) && text[i] != '{' && text[i] != '(' && text[i] != ';' && text[i] != ')')
            ++i;
        std::string tok = text.substr(start, i - start);
        if (tok.empty()) { ++i; continue; }

        if (isResult(tok)) { finish(); continue; }

        // Strip a leading move number: "12." "12..." "12.e4"
        size_t k = 0;
        while (k < tok.size() && std::isdigit((unsigned char)tok[k])) ++k;
        if (k > 0 && k < tok.size() && tok[k] == '.') {
            while (k < tok.size() && tok[k] == '.') ++k;
            tok = tok.substr(k);
        } else if (k == tok.size()) {
            continue;                                     // bare number
        }
        tok = stripAnnotations(tok);
        if (tok.find_first_not_of('.') == std::string::npos) continue;   // stray "..." token
        cur.sanMoves.push_back(tok);
    }
    finish();
    return games;
}

} // namespace wolf
