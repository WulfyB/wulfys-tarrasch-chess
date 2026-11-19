"""Line-by-line Python translation of Lichess's game accuracy (lila AccuracyPercent.gameAccuracy,
scalachess WinPercent, scalalib Maths), used to cross-check the C++ implementation.

    python3 lichess_reference.py path/to/wolf-tests [N]
"""
import math, random, subprocess, sys

def winning_chances(cp):
    cp = max(-1000, min(1000, cp))
    return max(-1.0, min(1.0, 2 / (1 + math.exp(-0.00368208 * cp)) - 1))

def win_percent(cp):
    return 50 + 50 * winning_chances(cp)

def from_win_percents(before, after):
    if after >= before:
        return 100.0
    raw = 103.1668100711649 * math.exp(-0.04354415386753951 * (before - after)) + -3.166924740191411
    return min(100.0, max(0.0, raw + 1))

def std_dev(a):
    m = sum(a) / len(a)
    return math.sqrt(sum((x - m) ** 2 for x in a) / len(a))

def sliding(lst, n):  # Scala List.sliding semantics
    if len(lst) <= n:
        return [lst[:]] if lst else []
    return [lst[i:i + n] for i in range(len(lst) - n + 1)]

def game_accuracy(cps, white_starts=True):
    wins = [win_percent(c) for c in cps]
    window_size = max(2, min(8, (len(cps) - 1) // 10))
    windows = [wins[:window_size]] * (min(window_size, len(wins)) - 2) + sliding(wins, window_size)
    weights = [max(0.5, min(12, std_dev(w))) for w in windows]
    pairs = sliding(wins, 2)
    weighted = []
    for i, (pair, w) in enumerate(zip(pairs, weights)):
        if len(pair) != 2:
            continue
        p, n = pair
        white = (i % 2 == 0) == white_starts
        acc = from_win_percents(p, n) if white else from_win_percents(100 - p, 100 - n)
        weighted.append((acc, w, white))
    out = []
    for side in (True, False):
        items = [(a, w) for a, w, c in weighted if c == side]
        if not items:
            out.append(None); continue
        wm = sum(a * w for a, w in items) / sum(w for _, w in items)
        hm = len(items) / sum(1 / max(1, a) for a, _ in items)
        out.append((wm + hm) / 2)
    return out

def main():
    exe = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    rng = random.Random(42)
    worst = 0.0
    for t in range(n):
        length = rng.randint(2, 140)
        cp, cps = rng.randint(-50, 50), []
        for _ in range(length):
            cps.append(cp)
            r = rng.random()
            cp += rng.randint(-30, 30) if r < 0.8 else rng.randint(-600, 600)
            if rng.random() < 0.03:
                cp = rng.choice([1000, -1000, 5000, -5000])
            cp = max(-20000, min(20000, cp))
        ws = rng.random() < 0.85
        ref = game_accuracy(cps, ws)
        res = subprocess.run([exe, "--game-accuracy", "W" if ws else "B", *map(str, cps)],
                             capture_output=True, text=True).stdout.split()
        for r_, c_ in zip(ref, res):
            if r_ is None or c_ == "none":
                assert (r_ is None) == (c_ == "none"), (t, cps, ref, res)
                continue
            worst = max(worst, abs(r_ - float(c_)))
    print(f"{n} random games compared, worst difference {worst:.2e}")
    assert worst < 1e-4, "mismatch!"

if __name__ == "__main__":
    main()
