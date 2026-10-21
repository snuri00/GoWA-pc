import sys


def main():
    path = sys.argv[1]
    lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x40000
    minc = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    rows = []
    for line in open(path):
        a, b = line.split()
        a = int(a, 16)
        if lo <= a < hi:
            rows.append((a, int(b)))
    total = sum(b for _, b in rows)
    runs = []
    for a, b in rows:
        if runs and runs[-1][1] + 4 == a and runs[-1][2] == b:
            runs[-1][1] = a
        else:
            runs.append([a, a, b])
    for s, e, c in runs:
        n = (e - s) // 4 + 1
        if c * n >= minc:
            print("%05x-%05x n %4d count %9d insns %10d (%5.2f%%)" % (s, e, n, c, c * n, 100.0 * c * n / max(1, total)))
    print("total", total)


if __name__ == "__main__":
    main()
