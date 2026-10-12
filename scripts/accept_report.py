#!/usr/bin/env python3
"""Acceptance report for lvc suggestion telemetry.

Reads ~/.lvc/suggestions.jsonl (JSONL: {"ts":..,"lang":..,"suggestion":..,
"accepted":true|false}) and prints shown/accepted/dismissed counts, the
acceptance rate, and a summary of the 10 most recent suggestions.
"""
import json
import os
import sys

LOG = os.path.expanduser("~/.lvc/suggestions.jsonl")

def main():
    if not os.path.exists(LOG):
        print(f"no telemetry log found at {LOG}")
        print("the editor records suggestions there once AI suggestions are shown")
        return 0

    shown = accepted = dismissed = 0
    entries = []
    with open(LOG, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
            except json.JSONDecodeError:
                continue
            shown += 1
            if e.get("accepted") is True:
                accepted += 1
            else:
                dismissed += 1
            entries.append(e)

    if shown == 0:
        print("log exists but contains no suggestions")
        return 0

    print(f"log: {LOG}")
    print(f"shown:     {shown}")
    print(f"accepted:  {accepted}")
    print(f"dismissed: {dismissed}")
    print(f"acceptance rate: {accepted / shown * 100:.1f}%")
    print()
    print("recent 10 suggestions:")
    for e in entries[-10:]:
        sugg = str(e.get("suggestion", "")).replace("\n", "\\n")
        if len(sugg) > 60:
            sugg = sugg[:57] + "..."
        status = "accepted" if e.get("accepted") is True else "dismissed"
        print(f"  [{e.get('ts', '?')}] {e.get('lang', '?'):8s} {status:9s} {sugg}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
