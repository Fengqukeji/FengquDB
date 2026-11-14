"""Run with python -m llmwiki --db <directory> <command>."""

import argparse
import json
import os
import sys
from pathlib import Path

from .providers import from_environment
from .server import WikiServer
from .wiki import Wiki


def main(argv=None):
    parser = argparse.ArgumentParser(description="Incremental LLM Wiki backed entirely by FengquDB")
    parser.add_argument("--db", default="wiki-data", help="FengquDB directory")
    parser.add_argument("--library", default=os.environ.get("FENGQUDB_LIBRARY"))
    parser.add_argument("--namespace", default="default")
    commands = parser.add_subparsers(dest="command", required=True)
    ingest = commands.add_parser("ingest", help="import one UTF-8 text/Markdown file")
    ingest.add_argument("file", type=Path)
    ingest.add_argument("--uri", help="stable logical id; defaults to resolved file URI")
    ingest.add_argument("--title")
    topic = commands.add_parser("topic", help="define a cross-source wiki page")
    topic.add_argument("id")
    topic.add_argument("title")
    topic.add_argument("source_ids", nargs="+")
    topic.add_argument("--link", action="append", default=[])
    compile_command = commands.add_parser("compile", help="drain currently due compilation jobs")
    compile_command.add_argument("--limit", type=int, default=100)
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
    listing.add_argument("kind", choices=["source", "page", "job", "spec", "event"])
    listing.add_argument("--limit", type=int, default=100)
    listing.add_argument("--after", default="")
    commands.add_parser("status")
    commands.add_parser("lint")
    commands.add_parser("compact")
    commands.add_parser("verify")
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
        with Wiki(args.db, library=args.library, namespace=args.namespace, provider=from_environment()) as wiki:
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
                # Read a bounded payload even when the file is unexpectedly enormous.
                with args.file.open("rb") as source_file:
                    content = source_file.read(256 * 1024 + 1).decode("utf-8-sig")
                result = wiki.ingest(args.uri or args.file.resolve().as_uri(), content, title=args.title or args.file.stem)
            elif args.command == "topic":
                result = wiki.define_page(args.id, args.title, args.source_ids, links=args.link)
            elif args.command == "compile":
                if not 1 <= args.limit <= 10000:
                    raise ValueError("compile limit must be between 1 and 10000")
                result = []
                for _ in range(args.limit):
                    item = wiki.run_once()
                    if item is None:
                        break
                    result.append(item)
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
            elif args.command == "list":
                result = wiki.listing(args.kind, limit=args.limit, after=args.after)
            elif args.command in ("export", "backup"):
                result = getattr(wiki, args.command)(args.destination)
            else:
                result = getattr(wiki, args.command)()
            print(json.dumps(result, ensure_ascii=False, indent=2))
            if args.command == "compile" and any("error" in item or item["state"] == "failed" for item in result):
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
