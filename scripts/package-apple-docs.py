"""Add bilingual C API guides and public headers to an Apple native bundle."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def write_docs(sdk):
    sources = {
        'docs/en/languages/apple-native.md': 'GettingStarted.md',
        'docs/en/api/c.md': 'API.md',
        'docs/languages/apple-native.md': '接入.md',
        'docs/api/c.md': 'API.zh-CN.md',
    }
    links = {
        '../api/c.md': 'API.md',
        '../../api/c.md': 'API.zh-CN.md',
        '../../languages/apple-native.md': '接入.md',
        '../en/languages/apple-native.md': 'GettingStarted.md',
        '../languages/c.md': 'GettingStarted.md',
        '../en/api/c.md': 'API.md',
    }
    for source, target in sources.items():
        text = (ROOT/source).read_text(encoding='utf-8')
        for old, new in links.items():
            if source.startswith('docs/languages/') or source == 'docs/api/c.md':
                if old == '../api/c.md': new = 'API.zh-CN.md'
                if old == '../languages/c.md': new = '接入.md'
            text = text.replace(']('+old+')', ']('+new+')')
        locale = 'en' if source.startswith('docs/en/') else 'zh-CN'
        url = 'https://github.com/Fengqukeji/FengquDB/blob/develop/docs/'+('en/' if locale=='en' else '')+'languages/unity.md'
        text = text.replace('](unity.md)', ']('+url+')')
        (sdk/target).write_text(text,encoding='utf-8')
    (sdk/'include').mkdir(exist_ok=True)
    for path in (ROOT/'include').iterdir():
        if path.suffix in ('.h','.hpp') and not path.name.startswith('fengqudb_diagnostics'):
            shutil.copy2(path,sdk/'include'/path.name)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    write_docs(args.output.resolve(strict=True))
