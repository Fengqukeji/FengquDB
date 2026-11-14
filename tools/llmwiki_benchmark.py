"""Small reproducible end-to-end workload, not a cross-database ranking."""

import argparse
import json
import os
import statistics
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from llmwiki import Wiki


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int((len(values) - 1) * fraction))]


def run(library, count, queries):
    with tempfile.TemporaryDirectory() as directory:
        with Wiki(Path(directory) / "db", library=library) as wiki:
            source = "FengquDB uses transactions, consistent snapshots and durable storage. 数据库支持中文知识检索和增量编译。\n"
            corpus = [source * 12 + "\nshard%04d raretoken%04d" % (i % 20, i) for i in range(count)]
            start = time.perf_counter()
            for i, body in enumerate(corpus):
                wiki.ingest("bench:" + str(i), body, title="Document " + str(i))
            ingest_seconds = time.perf_counter() - start
            start = time.perf_counter()
            compiled = 0
            while (result := wiki.run_once()) is not None:
                if result["state"] != "done":
                    raise RuntimeError("compile failed: " + str(result))
                compiled += 1
            compile_seconds = time.perf_counter() - start
            latencies, visits, truncated = [], [], 0
            classes = {"rare_term": [], "broad_chinese": []}
            for i in range(queries):
                query = ("raretoken%04d" % (i % count)) if i % 2 == 0 else "中文 增量编译"
                start = time.perf_counter()
                result = wiki.search(query)
                elapsed = (time.perf_counter() - start) * 1000
                latencies.append(elapsed)
                classes["rare_term" if i % 2 == 0 else "broad_chinese"].append(elapsed)
                visits.append(result["postings_visited"])
                truncated += int(result["truncated"])
                if not result["results"]:
                    raise RuntimeError("query returned no evidence")
            start = time.perf_counter()
            for i, body in enumerate(corpus):
                if wiki.ingest("bench:" + str(i), body, title="Document " + str(i))["changed"]:
                    raise RuntimeError("unchanged source was recompiled")
            unchanged_seconds = time.perf_counter() - start
            return {"sources": count, "source_characters": len(corpus[0]), "queries": queries, "compiled": compiled,
                "ingest_sources_per_second": round(count / ingest_seconds, 2),
                "compile_pages_per_second": round(compiled / compile_seconds, 2),
                "unchanged_sources_per_second": round(count / unchanged_seconds, 2),
                "search_ms": {"p50": round(statistics.median(latencies), 3), "p95": round(percentile(latencies, .95), 3),
                              "max": round(max(latencies), 3)},
                "query_classes_ms": {key: {"p50": round(statistics.median(values), 3),
                    "p95": round(percentile(values, .95), 3)} for key, values in classes.items() if values},
                "truncated_queries": truncated, "max_postings_visited": max(visits), "status": wiki.status(),
                "conditions": "single process; warm cache; reliable commits; offline extractive compiler; synthetic bilingual corpus"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", default=os.environ.get("FENGQUDB_LIBRARY"))
    parser.add_argument("--sources", type=int, default=200)
    parser.add_argument("--queries", type=int, default=100)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.sources <= 10000 or not 1 <= args.queries <= 10000:
        parser.error("sources and queries must be in 1..10000")
    result = run(args.library, args.sources, args.queries)
    content = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content + "\n", encoding="utf-8")
    print(content)


if __name__ == "__main__":
    main()
