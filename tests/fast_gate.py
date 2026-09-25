#!/usr/bin/env python3
"""Run an opt-in fast CTest command, or skip it with CTest's skip code."""
import os
import sys

if os.environ.get("VOXELFORGE_RUN_FAST") != "1":
    sys.exit(77)
if len(sys.argv) < 2:
    sys.exit(2)
os.execvp(sys.argv[1], sys.argv[1:])
