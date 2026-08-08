#!/usr/bin/env python3
"""scripts/deploy.py — deploy Firebase Hosting (frontend) + RTDB rules."""
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
FB = ["firebase", "deploy", "--only", "database,hosting"]

def main():
    print("=== Deploying to Firebase ===")
    print("  Project: esp32-electricity-counter\n")

    result = subprocess.run(FB, cwd=PROJECT_ROOT)

    if result.returncode == 0:
        print("\n=== Deploy complete ===")
        print("  Dashboard: https://esp32-electricity-counter.web.app")
    else:
        print("\n=== Deploy FAILED ===", file=sys.stderr)

    sys.exit(result.returncode)

if __name__ == "__main__":
    main()
