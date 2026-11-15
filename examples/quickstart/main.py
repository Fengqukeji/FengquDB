"""Run with a downloaded Python SDK copied beside this file as FengquDB/."""
import argparse
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path(__file__).resolve().parent/'FengquDB')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent/'save')
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    if sdk.name != 'FengquDB' or not (sdk/'fengqudb.zip').is_file():
        parser.error('Copy the Python SDK FengquDB folder beside main.py, or pass --sdk /path/to/FengquDB')
    sys.path.insert(0, str(sdk.parent))
    from FengquDB import Store

    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    with Store(root/'main') as db:
        db.put(b'student/7/name', 'Alice'.encode('utf-8'))
        db.put(b'student/7/score', (95).to_bytes(8, 'little', signed=True))
        name = db.get(b'student/7/name').decode('utf-8')
        score = int.from_bytes(db.get(b'student/7/score'), 'little', signed=True)
        assert (name, score) == ('Alice', 95)
        print(name, score)
        db.put(b'student/7/score', (96).to_bytes(8, 'little', signed=True), force_reliable=True)
    with Store(root/'main') as db:
        score = int.from_bytes(db.get(b'student/7/score'), 'little', signed=True)
        assert score == 96
        print('Reopened score:', score)
        db.delete(b'student/7/name')
        db.delete(b'student/7/score')
        db.sync()
        assert db.get(b'student/7/name') is None
        print('Deleted:', db.get(b'student/7/name'))


if __name__ == '__main__':
    main()
