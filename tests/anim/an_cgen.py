import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pm"))
import wws_job_cgen as W

if __name__ == "__main__":
    W.generate(0x84BE40, 0xAA70, "pm::gen::an", sys.argv[1], sys.argv[2])
