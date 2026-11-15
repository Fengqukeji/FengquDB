"""Render one self-contained API guide per language, ordered by business use."""
from collections import defaultdict
from pathlib import Path
import re

HERE = Path(__file__).resolve().parent
LABELS = {'c': 'C', 'cpp': 'C++', 'csharp': 'C#', 'unity': 'Unity', 'java': 'Java',
          'python': 'Python', 'node': 'Node.js / TypeScript', 'go': 'Go', 'rust': 'Rust'}
ORDER = ('read', 'save', 'modify', 'delete', 'batch', 'transaction', 'query', 'index',
         'durability', 'expiry', 'fastread', 'snapshot', 'backup', 'collections',
         'maintenance', 'watch', 'serialization', 'other')


def group(row):
    name = row['name'].lower().replace('_', '')
    owner = row['owner'].lower()
    if owner.startswith('fengqudb_sys'): return 'other'
    if name.startswith('fqdbstore'): name = name[len('fqdbstore'):]
    if name.startswith('txn'): return 'transaction'
    # Handle-specific operations stay with their use case, including get/put.
    if any(t in owner for t in ('wire', 'codec', 'entity')): return 'serialization'
    if any(t in owner for t in ('transaction', 'txn')): return 'transaction'
    if any(t in owner for t in ('snapshot', 'cursor')): return 'snapshot'
    if any(t in owner for t in ('reader', 'preparedkey', 'storekey', 'shared')): return 'fastread'
    if any(t in owner for t in ('watch', 'change')): return 'watch'
    if any(t in owner for t in ('query', 'builder', 'select', 'column', 'qcol', 'qcond', 'qagg', 'qrow')) or '::q' in owner or '.q.' in owner: return 'query'
    if re.split(r'\.|::', owner)[-1] in ('col', 'agg', 'cond', 'sort', 'row', 'rows', 'typedrow', 'q'): return 'query'
    if name in ('from', 'from_', 's64', 'u64', 'i64', 'sint64', 'f64', 'f32', 'text', 'count', 'countof', 'countdistinct', 'sum', 'avg', 'min', 'max', 'and', 'or', 'not'): return 'query'
    if name.startswith('operator'): return 'query'
    if any(t in name for t in ('backup', 'restore', 'recover', 'history')): return 'backup'
    if any(t in name for t in ('watch', 'changeack', 'changes', 'logcallback')): return 'watch'
    if any(t in name for t in ('writeoption', 'writeconfig', 'configurewrite', 'writestate', 'submit', 'commitpoll', 'commitwait', 'resetwrite', 'sync')) or 'commit' in owner: return 'durability'
    if 'index' in name: return 'index'
    if any(t in name for t in ('shared', 'preparekey', 'preparereader', 'preparevalue', 'prepareentity')): return 'fastread'
    if any(t in name for t in ('snapshot', 'cursor', 'scan')): return 'snapshot'
    if any(t in name for t in ('transaction', 'txnbegin', 'txncommit', 'savepoint', 'rollback')) or name in ('begin',): return 'transaction'
    if any(t in name for t in ('query', 'select', 'explain')): return 'query'
    if any(t in name for t in ('many', 'batch')): return 'batch'
    if any(t in name for t in ('patch', 'exchange', 'ifabsent', 'ifchanged', 'ifversion', 'addi64', 'addint64')): return 'modify'
    if any(t in name for t in ('expire', 'expiry', 'purge')): return 'expiry'
    if name.startswith(('hash', 'zset', 'list')) or name in ('execute', 'fqdbstoreexecute'): return 'collections'
    if any(t in name for t in ('checkpoint', 'compact', 'verify', 'runtime', 'configure', 'info')): return 'maintenance'
    if name.startswith(('get', 'read', 'tryget', 'load')) or name.startswith(('fqdbstoreget',)): return 'read'
    if name.startswith(('put', 'set', 'fqdbstoreput')): return 'save'
    if name.startswith(('delete', 'erase', 'remove', 'fqdbstoredelete')): return 'delete'
    return 'other'


def brief(row):
    name = row['name'].lower().replace('_', '')
    if name.startswith('fqdbstore'): name = name[len('fqdbstore'):]
    if row['kind'] == 'constructor': return '创建对象'
    if any(t in name for t in ('close', 'dispose', 'release')): return '释放对象或借用的资源'
    if 'rollback' in name: return '回退到保存点'
    if 'savepoint' in name: return '创建或操作事务保存点'
    if 'commit' in name: return '提交事务或查看提交结果'
    if 'explain' in name: return '查看查询计划'
    if 'get' in name or name.startswith(('read', 'tryget')): return '读取对应的数据或状态'
    if name.startswith(('put', 'set')): return '保存或设置数据'
    if any(t in name for t in ('delete', 'erase', 'remove')): return '删除记录或成员'
    return {'read': '读取数据', 'save': '保存数据', 'modify': '条件更新或局部修改',
            'delete': '删除数据', 'batch': '批量处理', 'transaction': '事务操作',
            'query': '查询构建或结果读取', 'index': '定义或使用索引',
            'durability': '控制可靠确认或提交等待', 'expiry': '到期数据操作',
            'fastread': '复用键、缓冲或共享字节', 'snapshot': '一致读取或范围遍历',
            'backup': '备份、恢复或历史保留', 'collections': '原生集合操作',
            'maintenance': '查看状态或维护数据库', 'watch': '消费变更或定位问题',
            'serialization': '实体或二进制编解码', 'other': '辅助操作'}[group(row)]


def table(rows):
    result = ['| 所属类型 | 函数与参数 | 用途 |', '| --- | --- | --- |']
    for row in sorted(rows, key=lambda r: (r['owner'], r['name'], r['signature'])):
        signature = row['signature'].replace('|', r'\|').replace('`', '&#96;')
        result.append(f"| `{row['owner'] or '全局'}` | `{signature}` | {brief(row)} |")
    return '\n'.join(result) + '\n'


def render(lang, rows):
    recipes = (HERE/'recipes'/f'{lang}.md').read_text(encoding='utf-8')
    parts = re.split(r'<!-- api:([a-z]+) -->', recipes)
    sections = {parts[i]: parts[i+1].strip() for i in range(1, len(parts), 2)}
    groups = defaultdict(list)
    types = defaultdict(list)
    for row in rows:
        if row['kind'] in ('method', 'constructor'):
            groups[group(row)].append(row)
        else:
            types[row['owner'] or '全局'].append(row)
    output = [f'# {LABELS[lang]} API 与调用例子',
              f'[接入文档](../languages/{lang}.md)',
              '从前面的常用操作开始。本页每节都包含用途、调用例子和对应函数；展开“函数与参数”可查完整重载。']
    for key in ORDER:
        if key not in sections and not groups[key]: continue
        body = sections.get(key, '## 其他辅助接口' if key == 'other' else '## '+key)
        output.append(body)
        if groups[key]:
            output.append(f'<details>\n<summary>函数与参数（{len(groups[key])}项）</summary>\n\n'+table(groups[key])+'\n</details>')
    output.append('## 参数类型、返回值和常量\n\n需要设置 options 或检查返回状态时在这里查字段。普通增删改查不需要逐项阅读。')
    for owner, entries in sorted(types.items()):
        lines = ['| 声明 |', '| --- |']
        for row in sorted(entries, key=lambda r: (r['name'], r['signature'])):
            signature = row['signature'].replace('|', r'\|').replace('`', '&#96;')
            lines.append(f'| `{signature}` |')
        output.append('<details>\n<summary>'+owner+'</summary>\n\n'+'\n'.join(lines)+'\n\n</details>')
    if lang == 'c':
        for name in ('store_select', 'store_query'):
            proto = (HERE.parents[1]/'proto'/f'{name}.proto').read_text(encoding='utf-8')
            output.append(f'<details>\n<summary>查询格式：{name}</summary>\n\n```protobuf\n{proto.strip()}\n```\n\n</details>')
    return '\n\n'.join(output) + '\n'
