#!/usr/bin/env python3
"""Compare time-managed behaviour between builds.

bench is fixed-depth and seq.py used `go depth`, so both are blind to how much
time the engine decides to spend. Two builds can be byte-identical at fixed
depth and still play differently under a clock. This drives a real game with a
real time control and records, per move, how long the engine actually thought
and how deep it got.
"""
import subprocess, sys, time, re

MOVES = ["e2e4","e7e5","g1f3","b8c6","f1b5","a7a6","b5a4","g8f6","e1g1","f8e7",
         "f1e1","b7b5","a4b3","d7d6","c2c3","e8g8"]

def run(binary, wtime, winc):
    p = subprocess.Popen([binary], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, bufsize=1)
    def send(s):
        p.stdin.write(s + "\n"); p.stdin.flush()
    send("uci")
    while "uciok" not in p.stdout.readline():
        pass
    send("setoption name Threads value 1")
    send("setoption name Hash value 64")
    send("isready")
    while "readyok" not in p.stdout.readline():
        pass
    send("ucinewgame")
    send("isready")
    while "readyok" not in p.stdout.readline():
        pass

    out = []
    for i in range(0, len(MOVES), 2):
        moves = " ".join(MOVES[:i])
        send(f"position startpos moves {moves}" if moves else "position startpos")
        t0 = time.time()
        send(f"go wtime {wtime} btime {wtime} winc {winc} binc {winc}")
        depth = nodes = 0
        while True:
            line = p.stdout.readline()
            if not line:
                break
            if line.startswith("info"):
                m = re.search(r"\bdepth (\d+)", line)
                if m:
                    depth = max(depth, int(m.group(1)))
                m = re.search(r"\bnodes (\d+)", line)
                if m:
                    nodes = int(m.group(1))
            if line.startswith("bestmove"):
                break
        out.append((round((time.time() - t0) * 1000), depth, nodes))
    send("quit")
    p.wait(timeout=10)
    return out

if __name__ == "__main__":
    wtime, winc = int(sys.argv[1]), int(sys.argv[2])
    builds = sys.argv[3:]
    res = {}
    for b in builds:
        res[b] = run(b, wtime, winc)
    print(f"tc: wtime={wtime} winc={winc}")
    for b in builds:
        tot_t = sum(r[0] for r in res[b])
        tot_n = sum(r[2] for r in res[b])
        print(f"\n{b}\n  total_ms={tot_t} total_nodes={tot_n}")
        print("  " + " ".join(f"{r[0]}ms/d{r[1]}" for r in res[b]))
