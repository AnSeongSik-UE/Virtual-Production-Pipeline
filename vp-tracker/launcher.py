"""Backward-compatible entry point for the pipeline supervisor."""

from supervisor import PipelineSupervisor as PipelineLauncher
from supervisor import main


if __name__ == "__main__":
    raise SystemExit(main())
