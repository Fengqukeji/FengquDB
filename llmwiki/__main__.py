"""Run with python -m llmwiki --db <directory> <command>."""

import argparse
import json
import os
import sys
from pathlib import Path

from .providers import from_environment
from .server import WikiServer
from .wiki import Wiki
from .native_index_provider import NativeIndexProvider
from .connectors import load_files


def main(argv=None):
    parser = argparse.ArgumentParser(description="Incremental LLM Wiki backed entirely by FengquDB")
    parser.add_argument("--db", default="wiki-data", help="FengquDB directory")
    parser.add_argument("--library", default=os.environ.get("FENGQUDB_LIBRARY"))
    parser.add_argument("--namespace", default="default")
    parser.add_argument("--index-provider", choices=("native",), default="native")
    parser.add_argument("--index-library", default=os.environ.get("FENGQUDB_INDEX_LIBRARY"))
    parser.add_argument("--index-space", default="fengqu-wiki-v1")
    parser.add_argument("--index-partition")
    commands = parser.add_subparsers(dest="command", required=True)
    ingest = commands.add_parser("ingest", help="import one UTF-8 text/Markdown file")
    ingest.add_argument("file", type=Path)
    ingest.add_argument("--uri", help="stable logical id; defaults to resolved file URI")
    ingest.add_argument("--title")
    batch = commands.add_parser("ingest-dir", help="atomically import a bounded UTF-8 directory")
    batch.add_argument("directory", type=Path)
    batch.add_argument("--uri-prefix")
    batch.add_argument("--request-key")
    topic = commands.add_parser("topic", help="define a cross-source wiki page")
    topic.add_argument("id")
    topic.add_argument("title")
    topic.add_argument("source_ids", nargs="+")
    topic.add_argument("--link", action="append", default=[])
    compile_command = commands.add_parser("compile", help="drain currently due compilation jobs")
    compile_command.add_argument("--limit", type=int, default=100)
    compile_command.add_argument("--workers", type=int, default=1)
    compile_command.add_argument("--timeout-ms", type=int, default=60000)
    for name in ("search", "ask"):
        command = commands.add_parser(name)
        command.add_argument("query")
        command.add_argument("--limit", type=int, default=6)
    page = commands.add_parser("page")
    page.add_argument("id")
    page.add_argument("--revision")
    source = commands.add_parser("source")
    source.add_argument("id")
    source.add_argument("--revision")
    delete = commands.add_parser("delete")
    delete.add_argument("source_id")
    retry = commands.add_parser("retry")
    retry.add_argument("page_id")
    listing = commands.add_parser("list")
    listing.add_argument("kind", choices=["source", "page", "job", "spec", "event", "entity", "claim", "conflict"])
    listing.add_argument("--limit", type=int, default=100)
    listing.add_argument("--after", default="")
    commands.add_parser("status")
    commands.add_parser("lint")
    commands.add_parser("compact")
    commands.add_parser("verify")
    for name in ("entity", "conflict"):
        command = commands.add_parser(name)
        command.add_argument("id")
    for name in ("revise", "release", "resolve"):
        command = commands.add_parser(name, help="apply a CAS-guarded JSON request file")
        command.add_argument("id")
        command.add_argument("request", type=Path)
    maintain = commands.add_parser("index-maintain")
    maintain.add_argument("operation", type=int, choices=(1, 2, 3, 4))
    for name in ("export", "backup"):
        command = commands.add_parser(name)
        command.add_argument("destination")
    serve = commands.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8098)
    serve.add_argument("--no-worker", action="store_true")
    serve.add_argument("--no-maintenance", action="store_true")
    args = parser.parse_args(argv)
    try:
        index = NativeIndexProvider(args.index_library, index_space=args.index_space.encode("utf-8"),
                 partition=args.index_partition.encode("utf-8") if args.index_partition is not None else None)
        specific = lambda prefix: from_environment(prefix) if (os.environ.get(prefix + "_MODEL_URL") or os.environ.get(prefix + "_MODEL")) else None
        with Wiki(args.db, library=args.library, namespace=args.namespace, provider=from_environment(), index_provider=index,
                  compiler=specific("WIKI_COMPILER"), answerer=specific("WIKI_ANSWERER")) as wiki:
            if args.command == "serve":
                with WikiServer((args.host, args.port), wiki, token=os.environ.get("WIKI_TOKEN"),
                                background=not args.no_worker, maintenance=not args.no_maintenance) as server:
                    print("FengquWiki listening on %s:%s" % server.server_address, flush=True)
                    try:
                        server.serve_forever(poll_interval=0.25)
                    except KeyboardInterrupt:
                        pass
                return 0
            if args.command == "ingest":
                item = load_files(args.file, wiki.limits)[0]
                result = wiki.ingest(args.uri or item["uri"], item["content"], title=args.title or item["title"])
            elif args.command == "ingest-dir":
                result = wiki.ingest_many(load_files(args.directory, wiki.limits, uri_prefix=args.uri_prefix),
                    request_key=args.request_key)
            elif args.command == "topic":
                result = wiki.define_page(args.id, args.title, args.source_ids, links=args.link)
            elif args.command == "compile":
                result = wiki.run_jobs(limit=args.limit, workers=args.workers, timeout_ms=args.timeout_ms)
            elif args.command in ("search", "ask"):
                result = getattr(wiki, args.command)(args.query, limit=args.limit)
            elif args.command == "page":
                result = wiki.page(args.id, revision=args.revision)
            elif args.command == "source":
                result = wiki.source(args.id, revision=args.revision)
            elif args.command == "delete":
                result = {"deleted": wiki.delete_source(args.source_id)}
            elif args.command == "retry":
                result = wiki.retry(args.page_id)
            elif args.command in ("entity", "conflict"):
                result = getattr(wiki, args.command)(args.id)
            elif args.command in ("revise", "release", "resolve"):
                with args.request.open("rb") as stream:
                    payload = stream.read(wiki.limits.page_bytes + 65537)
                if len(payload) > wiki.limits.page_bytes + 65536:
                    raise ValueError("request byte quota exceeded")
                body = json.loads(payload)
                method = {"revise": wiki.revise_page, "release": wiki.release_page, "resolve": wiki.resolve_conflict}[args.command]
                result = method(args.id, **body)
            elif args.command == "index-maintain":
                result = wiki.maintain_index(operation=args.operation)
            elif args.command == "list":
                result = wiki.listing(args.kind, limit=args.limit, after=args.after)
            elif args.command in ("export", "backup"):
                result = getattr(wiki, args.command)(args.destination)
            else:
                result = getattr(wiki, args.command)()
            print(json.dumps(result, ensure_ascii=False, indent=2))
            if args.command == "compile" and any("error" in item or item["state"] == "failed" for item in result["items"]):
                return 1
            if args.command == "lint" and not result["ok"]:
                return 1
            return 0
    except Exception as exc:
        # Do not print endpoints, credentials, document content, or provider response bodies.
        print("FengquWiki command failed: " + type(exc).__name__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
