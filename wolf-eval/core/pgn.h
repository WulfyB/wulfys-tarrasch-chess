// Minimal PGN reader: headers + main-line SAN moves (comments, variations and NAGs skipped).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace wolf {

struct PgnGame {
    std::map<std::string, std::string> headers;
    std::vector<std::string> sanMoves;   // main line only, annotations like !? stripped

    std::string header(const std::string &k, const std::string &dflt = "?") const {
        auto it = headers.find(k);
        return it == headers.end() ? dflt : it->second;
    }
};

std::vector<PgnGame> parsePgn(const std::string &text);
bool readFile(const std::string &path, std::string &out);

} // namespace wolf
