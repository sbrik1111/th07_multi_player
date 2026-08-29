"""UDP relay with latency, jitter and loss between the Host and each Guest (th20's tool, for any
number of Guests).

    python tools/udp_lag_proxy.py --host 28020 --proxy-base 29020 --guests 1,2,3 \
        --delay-ms 50 --jitter-ms 20 --loss 0.02 --stats wk/lag.json

The Host binds 127.0.0.1:host. Guest seat s binds 127.0.0.1:(host + s) and is given the peer
127.0.0.1:(proxy_base + s); the Host is given the peer 127.0.0.1:(proxy_base + 1) (seat s at
+ s - 1). The proxy socket proxy_base + s forwards the Guest's datagrams to the Host (so the
Host sees it as seat s's address) and the Host's replies back to the Guest, each delayed.
"""
import argparse
import heapq
import json
import random
import select
import socket
import time

ap = argparse.ArgumentParser()
ap.add_argument("--host", type=int, required=True)
ap.add_argument("--proxy-base", type=int, required=True)
ap.add_argument("--guests", default="1")
ap.add_argument("--delay-ms", type=float, default=50)
ap.add_argument("--jitter-ms", type=float, default=20)
ap.add_argument("--loss", type=float, default=0.02)
ap.add_argument("--seed", type=int, default=20260916)
ap.add_argument("--stats", required=True)
a = ap.parse_args()
rng = random.Random(a.seed)
host = ("127.0.0.1", a.host)
socks = {}
guest_addr = {}
for s in (int(x) for x in a.guests.split(",")):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", a.proxy_base + s))
    sock.setblocking(False)
    socks[sock] = s
    guest_addr[s] = ("127.0.0.1", a.host + s)
queue = []
seq = 0
stats = dict(received=0, dropped=0, forwarded=0, started=time.time())
last_stats = 0.0
while True:
    now = time.monotonic()
    timeout = max(0.0, min(0.005, queue[0][0] - now)) if queue else 0.005
    ready, _, _ = select.select(list(socks), [], [], timeout)
    for sock in ready:
        seat = socks[sock]
        while True:
            try:
                data, src = sock.recvfrom(65536)
            except (BlockingIOError, ConnectionResetError, OSError):
                break
            stats["received"] += 1
            if rng.random() < a.loss:
                stats["dropped"] += 1
                continue
            to = host if src != host else guest_addr[seat]
            if src != host:
                guest_addr[seat] = src
            delay = max(0.0, a.delay_ms + rng.uniform(-a.jitter_ms, a.jitter_ms)) / 1000.0
            seq += 1
            heapq.heappush(queue, (time.monotonic() + delay, seq, sock, to, data))
    now = time.monotonic()
    while queue and queue[0][0] <= now:
        _, _, sock, to, data = heapq.heappop(queue)
        try:
            sock.sendto(data, to)
            stats["forwarded"] += 1
        except OSError:
            pass
    if now - last_stats > 1.0:
        last_stats = now
        with open(a.stats, "w") as f:
            json.dump(stats, f)
