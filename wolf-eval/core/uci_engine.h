// Talks to a UCI chess engine (Stockfish etc.) running as a child process.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace wolf {

struct EngineResult {
    bool        ok = false;
    bool        isMate = false;
    int         value = 0;          // side-to-move POV: centipawns or mate distance
    int         depth = 0;
    std::string bestMoveUci;        // e.g. "e2e4", empty if none (terminal position)
    std::vector<std::string> pv;    // the engine's main line (UCI), starting with the best move
    // Second-best move (only when the engine runs with MultiPV 2 or more)
    bool        hasSecond = false;
    bool        secondIsMate = false;
    int         secondValue = 0;    // side-to-move POV
};

class UciEngine {
public:
    UciEngine();
    ~UciEngine();
    bool start(const std::string &path, std::string &err);
    std::string name() const { return name_; }
    bool setOption(const std::string &name, const std::string &value);
    void newGame();
    // Analyse a FEN to a fixed depth (or movetime in ms if > 0, which takes priority).
    // If onlyMove is set (UCI notation), the engine searches just that move ("searchmoves"),
    // so its score is the value of that move from the same root position.
    EngineResult analyse(const std::string &fen, int depth, int movetimeMs = 0, const std::string &onlyMove = "");
    void stop();

private:
    bool send(const std::string &line);
    bool readLine(std::string &line);
    bool waitFor(const std::string &prefix, std::string *lastLine = nullptr);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string name_;
};

} // namespace wolf
