"""Small reproducible 896-D native/RAG latency sample; no model or training."""
import argparse
import json
from pathlib import Path
import statistics
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--core", required=True)
parser.add_argument("--index", required=True)
parser.add_argument("--shared-python", required=True)
parser.add_argument("--rows", type=int, default=1000)
parser.add_argument("--queries", type=int, default=10)
parser.add_argument("--dimension", type=int, default=896)
parser.add_argument("--output", required=True)
args = parser.parse_args()
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(args.shared_python).resolve()))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "bindings/python"))
from fengqudb_rag import MemoryStore, MemoryWrite, MemorySearch, EmbeddingContract, CallbackEmbedding, QueryBudget
from fengqudb_rag.types import RequestGuard


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int((len(values) - 1) * fraction + 0.5))]


def measure(callback):
    times = []
    for i in range(args.queries):
        start = time.perf_counter()
        callback(i)
        times.append((time.perf_counter() - start) * 1000)
    return {"p50_ms": statistics.median(times), "p95_ms": percentile(times, 0.95), "max_ms": max(times), "samples_ms": times}


if not 1 <= args.rows <= 10000 or not 1 <= args.queries <= 100 or not 1 <= args.dimension <= 4096:
    raise ValueError("sample budget exceeded")
unit = [1.0] + [0.0] * (args.dimension - 1)
provider = CallbackEmbedding(lambda texts, query=False: [unit[:] for _ in texts],
                             EmbeddingContract("benchmark-synthetic-v1", args.dimension))
with tempfile.TemporaryDirectory() as root:
    with MemoryStore(root, embedding=provider, core_library=args.core, index_library=args.index) as store:
        start = time.perf_counter()
        for offset in range(0, args.rows, 128):
            store.put("benchmark", [MemoryWrite(text=f"code_{i} synthetic evidence for bounded retrieval", source_id=f"event:{i}",
                scope="benchmark", owner_id="actor") for i in range(offset, min(offset + 128, args.rows))],
                budget=QueryBudget(timeout_ms=60000))
        write_seconds = time.perf_counter() - start
        def search(i, mode):
            result = store.retrieve("benchmark", MemorySearch(scope="benchmark", owner_id="actor", include_public=False,
                query=f"code_{i % args.rows}", as_of=0, top_k=8, mode=mode))
            if result["termination"] != "finished" or result["index_coverage"] != "complete":
                raise RuntimeError("incomplete sample")
            return result
        # One warm request per mode is separate from measured requests.
        modes = {}
        for mode in ("dense", "lexical", "hybrid"):
            search(0, mode)
            modes[mode] = measure(lambda i, selected=mode: search(i, selected))
        with store._account("benchmark") as account, account.store.snapshot() as view:
            request = MemorySearch(scope="benchmark", owner_id="actor", include_public=False, query="code_0", as_of=0, top_k=8)
            state = account.repo.namespace(view, request.scope, request.owner_id, request.timeline)
            raw = store.runtime.embed([request.query], query=True)[0]
            budget = QueryBudget(candidate_budget=100000, scan_budget=250000)
            def native(_):
                return account.index.query(view, request, "actor", state, [], dense=True, raw_vector=raw,
                    vector_space=account.repo.vector_space, limit=8, budget=budget, guard=RequestGuard(budget),
                    remaining={k: getattr(budget, k) for k in ("posting_budget", "candidate_budget", "scan_budget")})
            native_only = measure(native)
        disk = sum(path.stat().st_size for path in Path(root).rglob("*") if path.is_file())
        report = dict(rows=args.rows, dimension=args.dimension, queries_per_mode=args.queries,
            write_seconds=write_seconds, writes_per_second=args.rows / write_seconds,
            reliable_batch=128, warm_modes=modes, native_dense=native_only, disk_bytes=disk,
            limits="Synthetic vectors; no model time, no zvec comparison, small warm sample, not a large-scale performance claim")
Path(args.output).parent.mkdir(parents=True, exist_ok=True)
Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({k: v for k, v in report.items() if k not in ("warm_modes", "native_dense")}, ensure_ascii=False))
print(json.dumps({k: {x: y for x, y in v.items() if x != "samples_ms"} for k, v in modes.items()}, ensure_ascii=False))
