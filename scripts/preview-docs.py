"""Serve the local project preview with UTF-8 text and rendered Markdown."""
import argparse
from functools import partial
import html
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import io
from pathlib import Path
import re
from urllib.parse import urlsplit

from markdown_it import MarkdownIt


STYLE = """
body{margin:0;color:#1f2328;background:#fff;font:16px/1.65 -apple-system,
BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif}
nav{padding:12px 24px;background:#f6f8fa;border-bottom:1px solid #d1d9e0}
nav span{margin-left:16px;color:#59636e;font-size:14px}
main{max-width:980px;margin:28px auto;padding:28px;border:1px solid #d1d9e0;
border-radius:6px}h1,h2{border-bottom:1px solid #d1d9e0;padding-bottom:.3em}
h1{font-size:30px}h2{margin-top:28px;font-size:24px}h3{font-size:20px}
a{color:#0969da;text-decoration:none}a:hover{text-decoration:underline}
img{max-width:100%;height:auto}p,li{overflow-wrap:break-word}
pre{padding:16px;background:#f6f8fa;overflow:auto;border-radius:6px}
code{font:14px/1.5 Consolas,monospace}p code,li code,td code{
background:#f6f8fa;padding:2px 5px;border-radius:4px}
table{border-collapse:collapse;display:block;overflow:auto;margin:16px 0}
td,th{padding:6px 13px;border:1px solid #d1d9e0}tr:nth-child(even){background:#f6f8fa}
blockquote{border-left:4px solid #d1d9e0;padding-left:16px;color:#59636e}
summary{cursor:pointer}details{margin:18px 0}
@media(max-width:700px){main{margin:0;border:0;padding:20px}}
"""


def render_markdown(text, home):
    parser = MarkdownIt("commonmark", {"html": True}).enable("table")
    seen = {}

    def heading(tokens, index, options, env):
        slug = re.sub(r"[^\w -]", "", tokens[index + 1].content.lower()).replace(" ", "-")
        count = seen.get(slug, 0)
        seen[slug] = count + 1
        tokens[index].attrSet("id", slug + ("-" + str(count) if count else ""))
        return parser.renderer.renderToken(tokens, index, options, env)

    parser.renderer.rules["heading_open"] = heading

    def link(tokens, index, options, env):
        href = tokens[index].attrGet("href")
        if href:
            target = urlsplit(href)
            if not target.scheme and not target.netloc and target.path.lower().endswith(".md"):
                tokens[index].attrSet("href", target._replace(path=target.path[:-3] + ".html").geturl())
        return parser.renderer.renderToken(tokens, index, options, env)

    parser.renderer.rules["link_open"] = link
    tokens = parser.parse(text)
    title = next((tokens[i + 1].content for i, token in enumerate(tokens)
                  if token.type == "heading_open"), "FengquDB")
    body = parser.renderer.render(tokens, parser.options, {})
    return ("<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
            "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>" + html.escape(title) + " · FengquDB</title><style>" + STYLE +
            "</style></head><body><nav><a href=\"" + html.escape(home, quote=True) +
            "\">FengquDB · 返回首页</a><span>文档预览</span></nav><main>" + body +
            "</main></body></html>").encode("utf-8")


class PreviewHandler(SimpleHTTPRequestHandler):
    def guess_type(self, path):
        content_type = super().guess_type(path)
        if content_type.startswith("text/") or content_type in (
                "application/json", "application/javascript", "application/xml"):
            return content_type + "; charset=utf-8"
        return content_type

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def send_head(self):
        request = urlsplit(self.path)
        source = Path(self.translate_path(request.path))
        markdown_request = source.suffix.lower() == ".md"
        if source.suffix.lower() == ".html" and not source.is_file():
            source = source.with_suffix(".md")
        elif not markdown_request:
            return super().send_head()
        root = Path(self.directory).resolve()
        if not source.resolve().is_relative_to(root) or not source.is_file():
            self.send_error(404, "Document not found")
            return None
        if markdown_request:
            self.send_response(302)
            self.send_header("Location", request._replace(path=request.path[:-3] + ".html").geturl())
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        home = "/artifacts/github-home-preview-20261115/zh.html"
        if not (root / home.lstrip("/")).is_file():
            home = "/README.zh-CN.md"
        try:
            data = render_markdown(source.read_text(encoding="utf-8-sig"), home)
        except (OSError, UnicodeError):
            self.send_error(500, "Could not read the UTF-8 document")
            return None
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        return io.BytesIO(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--port", type=int, default=7935)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    if not root.is_dir() or not 1 <= args.port <= 65535:
        parser.error("Use a project directory and a port in 1..65535")
    handler = partial(PreviewHandler, directory=str(root))
    with ThreadingHTTPServer(("127.0.0.1", args.port), handler) as server:
        print(f"FengquDB preview: http://127.0.0.1:{args.port}/", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
