#!/usr/bin/env python3
"""Read bounded BEP 9 task history from one Rats Search --data-dir.

Examples:
  python3 scripts/metadata_diagnostics.py DATA_DIR --failed --last 10
  python3 scripts/metadata_diagnostics.py DATA_DIR --hash 40_HEX_CHARS
  python3 scripts/metadata_diagnostics.py DATA_DIR --since 2026-11-07T00:00:00+08:00 --stages
  python3 scripts/metadata_diagnostics.py DATA_DIR --task RUN_ID-TASK_ID
"""

import argparse
import collections
import datetime as dt
import json
import sys
from pathlib import Path


def to_ms(value):
    if value is None:
        return None
    if value.isdecimal():
        return int(value)
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        raise argparse.ArgumentTypeError("time needs a UTC offset or Z")
    return int(parsed.timestamp() * 1000)


def read_tasks(data_dir):
    base = Path(data_dir) / "metadata_diagnostics" / "metadata_tasks.jsonl"
    paths = [Path(f"{base}.{n}") for n in (3, 2, 1)] + [base]
    tasks = {}
    for path in paths:
        if not path.is_file():
            continue
        with path.open(encoding="utf-8") as source:
            for line_no, line in enumerate(source, 1):
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    print(f"warning: skipped incomplete record {path}:{line_no}", file=sys.stderr)
                    continue
                task_id = row.get("id")
                if not task_id:
                    continue
                task = tasks.setdefault(task_id, {"id": task_id})
                event = row.get("event")
                if event == "started":
                    for key in ("task_id", "hash", "source", "started_at_ms", "supplied_peer"):
                        task.setdefault(key, row.get(key))
                elif event == "terminal":
                    task.update(row)
                elif event == "index_result":
                    task["index_result"] = row.get("index_result", "unknown")
                    task["index_result_at_ms"] = row.get("at_ms")
    # Rotation can discard a terminal record while retaining its later index
    # acknowledgement. That orphan is not evidence of a newly interrupted task.
    tasks = {key: task for key, task in tasks.items()
             if "started_at_ms" in task or "outcome" in task}
    for task in tasks.values():
        if "outcome" not in task:
            task["outcome"] = "interrupted_unknown"
            task["last_confirmed_stage"] = "unknown"
            task["root_cause"] = "unknown"
            task["index_result"] = "unknown"
    return list(tasks.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("data_dir", help="the instance --data-dir")
    parser.add_argument("--failed", action="store_true", help="timeout and explicit failure only")
    parser.add_argument("--hash", dest="info_hash", help="exact 40-character v1 InfoHash")
    parser.add_argument("--task", help="one unique task ID")
    parser.add_argument("--since", type=to_ms, help="ISO 8601 with zone, or epoch milliseconds")
    parser.add_argument("--until", type=to_ms, help="ISO 8601 with zone, or epoch milliseconds")
    parser.add_argument("--last", type=int, default=20, help="maximum task records (default: 20; 0: all)")
    parser.add_argument("--stages", action="store_true", help="summarize last confirmed stages of timeouts")
    args = parser.parse_args()
    tasks = read_tasks(args.data_dir)
    if args.failed:
        tasks = [t for t in tasks if t["outcome"] in ("timeout", "error")]
    if args.info_hash:
        tasks = [t for t in tasks if t.get("hash", "").lower() == args.info_hash.lower()]
    if args.task:
        tasks = [t for t in tasks if t["id"] == args.task]
    if args.since is not None:
        tasks = [t for t in tasks if t.get("started_at_ms", 0) >= args.since]
    if args.until is not None:
        tasks = [t for t in tasks if t.get("started_at_ms", 0) <= args.until]
    tasks.sort(key=lambda t: (t.get("started_at_ms", 0), t["id"]), reverse=True)
    if args.stages:
        counts = collections.Counter(t.get("last_confirmed_stage", "unknown")
                                     for t in tasks if t["outcome"] == "timeout")
        print(json.dumps({"timeout_stages": counts}, ensure_ascii=False, sort_keys=True))
    else:
        for task in tasks[:args.last or None]:
            print(json.dumps(task, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    main()
