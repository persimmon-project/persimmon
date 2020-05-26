#!/usr/bin/env python3
import subprocess
import sys

ROUNDS = 5
# PARAMS = [
#     (10000000, 0),
#     (10000000, 13),
#     (10000000, 26),
#     (12500000, 38),
#     (12500000, 51),
#     (15000000, 64),
#     (15000000, 77),
#     (23000000, 90),
#     (23000000, 102),
#     (30000000, 115),
#     (40000000, 128),
# ]
# NUM_WRITES = 100000000
NUM_WRITES = 50000000


def run_experiment(num_writes, stack_writes):
    out = subprocess.check_output(
        f"""
            sudo rm -rf /mnt/pmem1/bench &&
            sudo mkdir /mnt/pmem1/bench &&
            sudo numactl -m 1 ./membench_stack {num_writes*2} {stack_writes} &&
            sudo killall -9 membench_stack
        """, shell=True)
    _, dur_s, xput = out.decode("utf-8").strip().split(",")
    return dur_s, xput


def main():
    kind = sys.argv[1]
    for _ in range(ROUNDS):
        # for num_writes, stack_writes in PARAMS:
        for heap_writes in (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024):
            stack_writes = 1024 - heap_writes
            dur_s, xput = run_experiment(NUM_WRITES, stack_writes)
            print(f"{kind},{stack_writes},{dur_s},{xput}", flush=True)


if __name__ == "__main__":
    main()
