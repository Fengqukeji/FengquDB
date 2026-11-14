"""Bounded local UTF-8 connector. No URL fetching, symlinks or reparse traversal."""

import os
import stat
from pathlib import Path

from .limits import BudgetError, WikiError


def _regular(path, directory=False):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
        raise WikiError("file connector rejects symlinks and Windows reparse points")
    if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
        raise WikiError("connector requires a regular file/directory")
    return info


def _read(path, root, maximum):
    # Verify every path component, including a junction above the selected directory.
    for parent in (path, *path.parents):
        _regular(parent, directory=parent != path)
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root):
        raise WikiError("file escapes connector root")
    before = _regular(path)
    if before.st_size > maximum:
        raise BudgetError("file exceeds source byte quota")
    with path.open("rb") as stream:
        opened = os.fstat(stream.fileno())
        if (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            raise WikiError("file changed during connector admission")
        raw = stream.read(maximum + 1)
    after = _regular(path)
    if len(raw) > maximum:
        raise BudgetError("file exceeds source byte quota")
    if (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns) or path.resolve(strict=True) != resolved:
        raise WikiError("file changed while being read; retry")
    try:
        return raw.decode("utf-8-sig")
    except UnicodeError:
        raise WikiError("connector requires UTF-8 text") from None


def load_files(path, limits, *, uri_prefix=None, extensions=(".md", ".txt")):
    selected = Path(path).absolute()
    directory = selected.is_dir()
    _regular(selected, directory=directory)
    root = selected.resolve(strict=True) if directory else selected.parent.resolve(strict=True)
    if uri_prefix is not None and (not isinstance(uri_prefix, str) or not uri_prefix.strip() or len(uri_prefix) > 1024):
        raise WikiError("invalid connector URI prefix")
    files = []
    if directory:
        # Stop enumeration at the batch bound; never collect an unbounded tree in memory.
        nodes = [0]
        def walk(folder, depth=0):
            if depth > 32:
                raise BudgetError("connector directory depth exceeds 32")
            with os.scandir(folder) as entries:
                for entry in entries:
                    nodes[0] += 1
                    if nodes[0] > limits.batch_items * 64:
                        raise BudgetError("connector directory traversal quota exceeded")
                    current = Path(entry.path)
                    info = current.lstat()
                    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                        raise WikiError("connector tree contains a symlink/reparse point")
                    if entry.is_dir(follow_symlinks=False):
                        yield from walk(current, depth + 1)
                    elif entry.is_file(follow_symlinks=False) and current.suffix.lower() in extensions:
                        yield current
        visited = 0
        for current in walk(selected):
            visited += 1
            if visited > limits.batch_items:
                raise BudgetError("directory exceeds batch item quota; import explicit subdirectories")
            files.append(current)
    else:
        files = [selected]
    items, total = [], 0
    for current in sorted(files):
        content = _read(current, root, limits.source_bytes)
        total += len(content.encode())
        if total > limits.batch_bytes:
            raise BudgetError("connector exceeds batch byte quota")
        uri = (uri_prefix.rstrip("/") + "/" + current.resolve().relative_to(root).as_posix()) if uri_prefix else current.resolve().as_uri()
        items.append({"uri": uri, "content": content, "title": current.stem})
    if not items:
        raise WikiError("no UTF-8 text/Markdown files selected")
    return items
