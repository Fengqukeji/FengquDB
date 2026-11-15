"""Check the language of public guides, API coverage, and local navigation."""
from collections import Counter
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
LANGUAGES=('c','cpp','csharp','unity','java','python','node','go','rust')


def links(page):
    text=page.read_text(encoding='utf-8')
    assert '\ufffd' not in text, f'Invalid text: {page}'
    for link in re.findall(r'\[[^\]]+\]\(([^)]+)\)',text):
        if not re.match(r'[A-Za-z][A-Za-z0-9+.-]*:',link) and not link.startswith('#'):
            target=page.parent/link.split('#')[0]
            assert target.exists(), f'{page}: broken link {link}'


def main():
    comments=json.loads((ROOT/'tools/api/signature_comments.en.json').read_text(encoding='utf-8'))
    manifest=json.loads((ROOT/'docs/api/manifest.json').read_text(encoding='utf-8'))
    pages=[ROOT/'README.md',ROOT/'README.zh-CN.md']
    for directory in ('docs/languages','docs/api','docs/en/languages','docs/en/api'):
        pages.extend((ROOT/directory).glob('*.md'))
    for page in pages: links(page)
    for page in (ROOT/'docs/en').rglob('*.md'):
        text=page.read_text(encoding='utf-8').replace('简体中文','').replace('接入.md','')
        assert not re.search(r'[\u4e00-\u9fff]',text), f'Untranslated prose: {page}'
    counts={}
    for lang in LANGUAGES:
        zh=(ROOT/'docs/api'/f'{lang}.md').read_text(encoding='utf-8')
        en=(ROOT/'docs/en/api'/f'{lang}.md').read_text(encoding='utf-8')
        # Compare every inventoried overload, field and constant, including
        # escaped operators. Only source comments may differ between editions.
        rows=manifest['csharp' if lang=='unity' else lang]['entries']
        for row in rows:
            signature=row['signature'].replace('|',r'\|').replace('`','&#96;')
            assert f'`{signature}`' in zh, (lang,'missing Chinese API',row['name'])
            translated=row['signature']
            for original, english in comments.items(): translated=translated.replace(original,english)
            translated=translated.replace('|',r'\|').replace('`','&#96;')
            assert f'`{translated}`' in en, (lang,'missing English API',row['name'])
        assert Counter(re.findall(r'<summary>.*?\((\d+)\)',en))==Counter(re.findall(r'<summary>.*?（(\d+)项）',zh)), lang
        counts[lang]=len(rows)
    readme=(ROOT/'README.md').read_text(encoding='utf-8')
    for lang in LANGUAGES:
        assert f'](docs/en/languages/{lang}.md)' in readme
        assert f'](docs/en/api/{lang}.md)' in readme
        assert f'](docs/languages/{lang}.md)' not in readme
        assert f'](docs/api/{lang}.md)' not in readme
    print(json.dumps({'pages_checked':len(pages),'bilingual_api_entries':counts}))


if __name__=='__main__': main()
