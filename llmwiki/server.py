"""Small bounded HTTP JSON service. Run behind TLS when exposed remotely."""

import hmac
import ipaddress
import json
import logging
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .providers import ProviderError
from .storage import Error, encode
from .wiki import WikiError
from .limits import BudgetError, ConflictError

LOG = logging.getLogger("llmwiki")
MAX_BODY = 2 * 1024 * 1024


class WikiServer(ThreadingHTTPServer):
    # Drain in-flight handlers before the CLI closes its shared database handle.
    daemon_threads = False
    allow_reuse_address = True

    def __init__(self, address, wiki, *, token=None, background=True, maintenance=True, max_requests=24):
        host = address[0]
        try:
            loopback = ipaddress.ip_address(host).is_loopback
        except ValueError:
            loopback = host.lower() == "localhost"
        if not loopback and not token:
            raise WikiError("WIKI_TOKEN is required for a non-loopback listener")
        if token is not None and (not isinstance(token, str) or len(token) < 24 or not token.isascii()):
            raise WikiError("WIKI_TOKEN must contain at least 24 ASCII characters")
        self.wiki, self.token = wiki, token
        self.slots = threading.BoundedSemaphore(max_requests)
        self.stopping = threading.Event()
        self.worker = None
        if maintenance:
            wiki.enable_maintenance()
        super().__init__(address, Handler)
        if background:
            self.worker = threading.Thread(target=self._work, name="wiki-compiler", daemon=True)
            self.worker.start()

    def _work(self):
        while not self.stopping.is_set():
            try:
                result = self.wiki.run_once()
            except Exception as exc:
                LOG.error("compiler iteration failed: %s", type(exc).__name__)
                result = None
            if not result:
                self.stopping.wait(0.5)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            try:
                request.sendall(b"HTTP/1.0 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n")
            finally:
                self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()

    def server_close(self):
        self.stopping.set()
        super().server_close()
        if self.worker:
            self.worker.join()


class Handler(BaseHTTPRequestHandler):
    server_version = "FengquWiki/0.2"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, _format, *_args):
        # Query text, source data and authorization never go into access logs.
        pass

    def _json(self, status, value):
        data = encode(value)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        if self.headers.get("Transfer-Encoding"):
            raise WikiError("transfer encoding is unsupported")
        if self.headers.get_content_type() != "application/json":
            raise WikiError("Content-Type must be application/json")
        length = int(self.headers.get("Content-Length", "0"))
        if not 0 < length <= MAX_BODY:
            raise BudgetError("JSON body must contain 1..2097152 bytes")
        raw = self.rfile.read(length)
        if len(raw) != length:
            raise WikiError("incomplete JSON body")
        result = json.loads(raw)
        if not isinstance(result, dict):
            raise WikiError("JSON body must be an object")
        return result

    def _dispatch(self):
        if len(self.path) > 10000:
            raise WikiError("URL too long")
        target = urllib.parse.urlsplit(self.path)
        path = target.path.strip("/").split("/")
        query = urllib.parse.parse_qs(target.query, max_num_fields=16)
        get = lambda name, default=None: query.get(name, [default])[0]
        if self.command == "GET" and path == ["health"]:
            return 200, {"ok": True}
        token = self.server.token
        if token and not hmac.compare_digest(self.headers.get("Authorization", ""), "Bearer " + token):
            return 401, {"error": "unauthorized"}
        wiki = self.server.wiki
        if self.command == "GET":
            if path == ["v1", "status"]:
                return 200, wiki.status()
            if path == ["v1", "search"]:
                return 200, wiki.search(get("q"), limit=int(get("limit", "10")))
            if path == ["v1", "lint"]:
                return 200, wiki.lint()
            collections = {"sources": "source", "pages": "page", "jobs": "job", "events": "event", "specs": "spec",
                           "entities": "entity", "claims": "claim", "conflicts": "conflict"}
            if len(path) == 2 and path[0] == "v1" and path[1] in collections:
                return 200, wiki.listing(collections[path[1]], limit=int(get("limit", "100")), after=get("after", ""))
            if len(path) == 3 and path[:2] == ["v1", "sources"]:
                value = wiki.source(path[2], revision=get("revision"), version=int(get("version")) if get("version") else None)
                return (200, value) if value else (404, {"error": "source not found"})
            if len(path) == 3 and path[:2] == ["v1", "pages"]:
                value = wiki.page(path[2], revision=get("revision"))
                return (200, value) if value else (404, {"error": "page not found"})
            if len(path) == 3 and path[:2] in (["v1", "entities"], ["v1", "conflicts"]):
                value = (wiki.entity(path[2]) if path[1] == "entities" else
                         wiki.conflict(path[2], generation=int(get("generation")) if get("generation") else None))
                return (200, value) if value else (404, {"error": "record not found"})
        if self.command == "POST":
            body = self._body()
            if path == ["v1", "sources"]:
                return 200, wiki.ingest(**body)
            if path == ["v1", "sources", "batch"]:
                return 200, wiki.ingest_many(body.get("items"), request_key=body.get("request_key"))
            if path == ["v1", "sources", "delete-batch"]:
                return 200, wiki.delete_sources(body.get("items"), request_key=body.get("request_key"))
            if path == ["v1", "search"]:
                return 200, wiki.search(**body)
            if path == ["v1", "pages"]:
                return 202, wiki.define_page(body.get("id"), body.get("title"), body.get("source_ids"), links=body.get("links"))
            if path == ["v1", "ask"]:
                return 200, wiki.ask(body.get("question"), limit=body.get("limit", 6),
                    context_chars=body.get("context_chars", 24000), **body.get("search", {}))
            if path == ["v1", "jobs", "run"]:
                return 200, wiki.run_jobs(**body)
            if path == ["v1", "index", "maintain"]:
                return 200, wiki.maintain_index(**body)
            if path == ["v1", "vectors"]:
                return 200, wiki.set_vectors(**body)
            if len(path) == 4 and path[:2] == ["v1", "pages"] and path[3] in ("revise", "release"):
                return 200, (wiki.revise_page if path[3] == "revise" else wiki.release_page)(path[2], **body)
            if len(path) == 4 and path[:2] == ["v1", "conflicts"] and path[3] == "resolve":
                return 200, wiki.resolve_conflict(path[2], **body)
            if len(path) == 4 and path[:2] == ["v1", "jobs"] and path[3] == "retry":
                return 202, wiki.retry(path[2])
        if self.command == "DELETE" and len(path) == 3 and path[:2] == ["v1", "sources"]:
            return 200, {"deleted": wiki.delete_source(path[2],
                expected_version=int(get("expected_version")) if get("expected_version") is not None else None,
                expected_revision=get("expected_revision"))}
        return 404, {"error": "route not found"}

    def _handle(self):
        try:
            status, result = self._dispatch()
            self._json(status, result)
        except ConflictError:
            self._json(409, {"error": "observed version or idempotency key conflicts with current state"})
        except BudgetError:
            self._json(413, {"error": "application quota exceeded; split the request"})
        except (ValueError, TypeError, OverflowError):
            self._json(400, {"error": "invalid request"})
        except ProviderError:
            self._json(502, {"error": "model response unavailable or invalid"})
        except Error as exc:
            self._json(503, {"error": "storage operation failed", "code": exc.code})
        except (TimeoutError, ConnectionError):
            self.close_connection = True
        except Exception as exc:
            LOG.error("HTTP operation failed: %s", type(exc).__name__)
            self._json(500, {"error": "internal error"})

    do_GET = do_POST = do_DELETE = _handle
