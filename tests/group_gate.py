#!/usr/bin/env python3
"""Run a CTest command only when one of its groups is explicitly enabled.

The repository deliberately has no all-tests target.  CTest still knows about
all checks so focused groups can be inspected with ``ctest -N``/``-L``, but a
bare CTest invocation cannot start an expensive gate by accident.
"""
from __future__ import annotations

import os
import sys


def _groups(value: str) -> set[str]:
    return {item for item in value.replace(",", ";").split(";") if item}


def main() -> int:
    if len(sys.argv) < 3:
        print(
            f"usage: {sys.argv[0]} GROUP[,GROUP...] COMMAND [ARG ...]",
            file=sys.stderr,
        )
        return 2

    requested = _groups(sys.argv[1])
    enabled = _groups(os.environ.get("VOXELFORGE_TEST_GROUPS", ""))
    # Keep the old fast-profile invocation useful without allowing it to
    # unlock the heavyweight groups: VOXELFORGE_RUN_FAST=1 means smoke only.
    if os.environ.get("VOXELFORGE_RUN_FAST") == "1":
        enabled.add("smoke")
    if not requested or not (requested & enabled):
        print(
            f"group gate: {','.join(sorted(requested))} not enabled "
            f"(VOXELFORGE_TEST_GROUPS={','.join(sorted(enabled)) or 'unset'})",
            file=sys.stderr,
        )
        return 77

    command = sys.argv[2:]
    os.execvp(command[0], command)
    return 0  # unreachable


if __name__ == "__main__":
    raise SystemExit(main())
