"""Download official benchmark-only sources and validate pinned source hashes."""
import hashlib
import io
import json
from pathlib import Path
from urllib.request import Request, urlopen
import zipfile

ROOT = Path(__file__).resolve().parents[1]/'artifacts/reference'


def fetch(url):
    with urlopen(Request(url, headers={'User-Agent': 'Mozilla/5.0'}), timeout=120) as response:
        return response.read()


def checked(data, sha):
    assert hashlib.sha256(data).hexdigest() == sha, 'Source hash mismatch'
    return data


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() != data:
        raise RuntimeError('Refuse to replace different reference input: '+str(path))
    path.write_bytes(data)


def main():
    url = 'https://download.redis.io/releases/redis-8.10.3.tar.gz'
    save(ROOT/'redis-8.10.3.tar.gz', checked(fetch(url),
         'edde778f6bdee7581e42fc139ca9e07e2822212092f7501f99989656e3629d02'))
    url = 'https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip'
    archive = fetch(url)
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        for name, sha in (
            ('sqlite3.c','0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52'),
            ('sqlite3.h','d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c')):
            save(ROOT/'sqlite-amalgamation-3540100'/name,
                 checked(package.read('sqlite-amalgamation-3540100/'+name), sha))
    origin = dict(url=url,archive_sha256=hashlib.sha256(archive).hexdigest(),version='3.54.1')
    path=ROOT/'sqlite-amalgamation-3540100/origin.json'
    if not path.exists():save(path,(json.dumps(origin,indent=2)+'\n').encode())
    files = []
    for name, suffix, sha in (
        ('yyjson.c','src/yyjson.c','ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a'),
        ('yyjson.h','src/yyjson.h','175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3'),
        ('LICENSE','LICENSE','45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947')):
        url='https://raw.githubusercontent.com/ibireme/yyjson/0.12.0/'+suffix
        save(ROOT/'yyjson-0.12.0'/name,checked(fetch(url),sha))
        files.append(dict(file=name,url=url,sha256=sha))
    origin=ROOT/'yyjson-0.12.0/origin.json'
    if not origin.exists():save(origin,(json.dumps(dict(version='0.12.0',files=files),indent=2)+'\n').encode())
    print('Official Redis / SQLite / yyjson sources verified.')


if __name__ == '__main__':main()
