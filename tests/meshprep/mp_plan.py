import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp_cgen


def main():
    job = sys.argv[1]
    mp_cgen.configure(job)
    sys.argv = [os.path.join(HERE, "..", "renjob", "rj_plan.py")] + sys.argv[2:]
    runpy.run_path(sys.argv[0], run_name="__main__")


if __name__ == "__main__":
    main()
