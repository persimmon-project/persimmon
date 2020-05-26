#!/usr/bin/env python3
import subprocess
import sys

ROUNDS = 5
PARAMS = [
    (100000000, 0),
    (100000000, 1),
    (100000000, 2),
    (100000000, 4),
    (100000000, 8),
    (100000000, 16),
    (100000000, 32),
    (50000000, 64),
    (50000000, 128),
    (50000000, 256),
    (20000000, 512),
    (10000000, 1024),
]


def run_experiment(num_writes, advance):
    out = subprocess.check_output(
        f"""
            sudo rm -rf /mnt/pmem1/bench &&
            sudo mkdir /mnt/pmem1/bench &&
            sudo numactl -m 1 ./membench_batch {num_writes*2} {advance} &&
            sudo killall -9 membench_batch
        """, shell=True)
    _, dur_s, xput = out.decode("utf-8").strip().split(",")
    return dur_s, xput


def main():
    kind = sys.argv[1]
    for _ in range(ROUNDS):
        for num_writes, advance in PARAMS:
            if kind == "no-batch":
                num_writes = 10000000
            dur_s, xput = run_experiment(num_writes, advance)
            print(f"{kind},{advance},{dur_s},{xput}", flush=True)


if __name__ == "__main__":
    main()
