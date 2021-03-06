#!/usr/bin/env python3
import subprocess
import sys

ROUNDS = 5
PARAMS = [
    (12000000, 1024),
    (25000000, 512),
    (50000000, 256),
    (100000000, 128),
    (100000000, 64),
    (200000000, 32),
    (200000000, 16),
    (200000000, 8),
    (400000000, 4),
    (400000000, 2),
    (400000000, 1),
]


def run_experiment(num_writes, nlocs):
    out = subprocess.check_output(
        f"""
            sudo rm -rf /mnt/pmem1/bench &&
            sudo mkdir /mnt/pmem1/bench &&
            sudo numactl -m 1 ./membench_dedup psm {num_writes*2} {nlocs} &&
            sudo killall -9 membench_dedup
        """, shell=True)
    _, dur_s, xput = out.decode("utf-8").strip().split(",")
    return dur_s, xput


def main():
    kind = sys.argv[1]
    for _ in range(ROUNDS):
        for num_writes, nlocs in PARAMS:
            if kind == "no-dedup":
                num_writes = 10000000
            dur_s, xput = run_experiment(num_writes, nlocs)
            print(f"{kind},{nlocs},{dur_s},{xput}", flush=True)


if __name__ == "__main__":
    main()
