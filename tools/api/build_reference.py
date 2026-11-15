"""Build per-language API guides from compiler inventories and business examples.

Compiler inputs live in build/api-*.json. The indexes are shipped documentation;
this development tool and compiler ASTs are never part of a client package.
"""
import ast
from bisect import bisect_right
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
BUILD=ROOT/'build'


def load(name):
    text=(BUILD/name).read_text(encoding='utf-8-sig')
    # dotnet may emit build diagnostics before its JSON payload.
    if not text.lstrip().startswith(('[','{')):text=text[text.index('[{'):]
    return json.loads(text)


def stream_json(path):
    data=path.read_text(encoding='utf-8-sig');decoder=json.JSONDecoder();p=0
    while p<len(data):
        if data[p].isspace():p+=1;continue
        if data.startswith('Dumping ',p):p=data.index('\n',p)+1;continue
        value,p=decoder.raw_decode(data,p);yield value


def compiler_cpp(filename,language):
    rows=[];current_file='';lines={};sources={}
    def source_text(node,path):
        span=node.get('range',{});start=span.get('begin',{});end=span.get('end',{})
        if 'offset' not in start or 'offset' not in end or not path:return ''
        if path not in sources:sources[path]=(ROOT/path).read_bytes()
        return sources[path][start['offset']:end['offset']+end.get('tokLen',1)].decode('utf-8')
    def visit(node,owner='',path='',access='public',template=''):
        kind=node.get('kind');name=node.get('name','');loc=node.get('loc',{})
        path=loc.get('file',path)
        path=path.replace('\\','/')
        if '/include/fengqudb' in path:path='include/'+path.split('/include/')[-1]
        elif path.startswith('include/'):pass
        elif path and not path.startswith('include/'):return
        if node.get('isImplicit'):return
        if path and 'offset' in loc and 'line' not in loc:
            if path not in lines:lines[path]=[i for i,b in enumerate((ROOT/path).read_bytes()) if b==10]
            loc=dict(loc,line=bisect_right(lines[path],loc['offset'])+1)
        if kind=='NamespaceDecl':
            if name=='detail':return
            owner=(owner+'::' if owner else '')+name
        if kind in ('FunctionTemplateDecl','ClassTemplateDecl'):
            params=[source_text(c,path) for c in node.get('inner',[]) if c.get('kind') in ('TemplateTypeParmDecl','NonTypeTemplateParmDecl','TemplateTemplateParmDecl')]
            template='template <'+', '.join(params)+'> '
        if kind=='EnumDecl' and name and access=='public' and path:
            rows.append(dict(language=language,owner=owner,name=name,kind='type',signature=('enum class ' if node.get('scopedEnumTag') else 'enum ')+name,path=path,line=loc.get('line',1)))
            if node.get('scopedEnumTag'):owner=(owner+'::' if owner else '')+name
        if kind in ('CXXRecordDecl','RecordDecl'):
            if not name:return
            if access!='public':return
            rows.append(dict(language=language,owner=owner,name=name,kind='type',signature=template+node.get('tagUsed','struct')+' '+name,path=path,line=loc.get('line',1)))
            owner=(owner+'::' if owner else '')+name
            access='private' if node.get('tagUsed')=='class' else 'public'
        if kind in ('FunctionDecl','CXXMethodDecl','CXXConstructorDecl','CXXConversionDecl','CXXDestructorDecl'):
            if access=='public' and path:
                params=[source_text(p,path) or p['type']['qualType']+' '+p.get('name','') for p in node.get('inner',[]) if p.get('kind')=='ParmVarDecl']
                ty=node.get('type',{}).get('qualType','');split=ty.find('(');ret=ty[:split].strip() if split>=0 else ty
                suffix=ty[ty.rfind(')')+1:] if ')' in ty else ''
                if kind in ('CXXConstructorDecl','CXXConversionDecl','CXXDestructorDecl'):ret=''
                signature=template+(ret+' '+name+'('+', '.join(params)+')'+suffix).strip()
                rows.append(dict(language=language,owner=owner,name=name,kind='method',signature=signature,path=path,line=loc.get('line',1)))
            return
        if kind in ('FieldDecl','EnumConstantDecl','VarDecl','TypedefDecl','TypeAliasDecl') and access=='public' and name and path:
            ty=node.get('type',{}).get('qualType','');value=next((x.get('value') for x in node.get('inner',[]) if 'value' in x),None)
            signature=ty+' '+name+(' = '+str(value) if value is not None else '')
            rows.append(dict(language=language,owner=owner,name=name,kind='field' if kind=='FieldDecl' else 'constant' if kind=='EnumConstantDecl' else 'type',signature=signature,path=path,line=loc.get('line',1)))
        current=access
        for child in node.get('inner',[]):
            if child.get('kind')=='AccessSpecDecl':current=child['access'];continue
            visit(child,owner,path,current,template if kind in ('FunctionTemplateDecl','ClassTemplateDecl') else '')
    for item in stream_json(BUILD/filename):
        current_file=item.get('loc',{}).get('file',current_file)
        visit(item,path=current_file)
    return rows


def python_rows():
    rows=[]
    for path in (ROOT/'bindings/python').glob('*.py'):
        tree=ast.parse(path.read_text(encoding='utf-8'));relative=path.relative_to(ROOT).as_posix()
        definitions={n.name:n for n in tree.body if isinstance(n,(ast.ClassDef,ast.FunctionDef,ast.AsyncFunctionDef))}
        def visit(nodes,owner=path.stem):
            for n in nodes:
                if isinstance(n,ast.ClassDef) and not n.name.startswith('_'):
                    rows.append(dict(language='python',owner=owner,name=n.name,kind='type',signature='class '+n.name,path=relative,line=n.lineno))
                    visit(n.body,owner+'.'+n.name)
                    for base in n.bases:
                        if isinstance(base,ast.Name) and base.id=='_Handle' and base.id in definitions:
                            visit([m for m in definitions[base.id].body if isinstance(m,ast.FunctionDef) and m.name in ('close','__enter__','__exit__')],owner+'.'+n.name)
                elif isinstance(n,(ast.FunctionDef,ast.AsyncFunctionDef)) and (not n.name.startswith('_') or (n.name.startswith('__') and n.name.endswith('__'))):
                    rows.append(dict(language='python',owner=owner,name=n.name,kind='method',signature=('async ' if isinstance(n,ast.AsyncFunctionDef) else '')+'def '+n.name+'('+ast.unparse(n.args)+')'+(' -> '+ast.unparse(n.returns) if n.returns else ''),path=relative,line=n.lineno))
                    if n.name=='__init__':
                        for statement in ast.walk(n):
                            if isinstance(statement,(ast.Assign,ast.AnnAssign)):
                                for target in statement.targets if isinstance(statement,ast.Assign) else [statement.target]:
                                    if isinstance(target,ast.Attribute) and isinstance(target.value,ast.Name) and target.value.id=='self' and not target.attr.startswith('_'):
                                        rows.append(dict(language='python',owner=owner,name=target.attr,kind='property',signature=target.attr,path=relative,line=statement.lineno))
                elif isinstance(n,ast.AnnAssign) and isinstance(n.target,ast.Name) and owner!=path.stem and not n.target.id.startswith('_'):
                    rows.append(dict(language='python',owner=owner,name=n.target.id,kind='field',signature=ast.unparse(n),path=relative,line=n.lineno))
                elif isinstance(n,ast.Assign):
                    for target in n.targets:
                        if isinstance(target,ast.Name) and owner!=path.stem:
                            if not target.id.startswith('_'):rows.append(dict(language='python',owner=owner,name=target.id,kind='constant',signature=target.id+' = '+ast.unparse(n.value),path=relative,line=n.lineno))
                            elif target.id=='_fields_' and isinstance(n.value,ast.List):
                                for field in n.value.elts:
                                    if isinstance(field,ast.Tuple) and len(field.elts)>=2 and isinstance(field.elts[0],ast.Constant):
                                        rows.append(dict(language='python',owner=owner,name=field.elts[0].value,kind='field',signature=str(field.elts[0].value)+': '+ast.unparse(field.elts[1]),path=relative,line=n.lineno))
                        if isinstance(target,ast.Attribute) and isinstance(target.value,ast.Name) and not target.value.id.startswith('_') and not target.attr.startswith('_'):
                            body=definitions.get(n.value.id) if isinstance(n.value,ast.Name) else None
                            signature='def '+target.attr+'('+ast.unparse(body.args)+')' if isinstance(body,ast.FunctionDef) else ast.unparse(target)+' = '+ast.unparse(n.value)
                            rows.append(dict(language='python',owner=path.stem+'.'+target.value.id,name=target.attr,kind='method',signature=signature,path=relative,line=n.lineno))
        visit(tree.body)
    return rows


def rust_rows(crate='fengqudb'):
    doc=load('api-rust-target/doc/'+crate+'.json');items=doc['index'];owners={};public_variants=set()
    for item in items.values():
        inner=item['inner']
        if 'impl' in inner:
            impl=inner['impl'];ty=impl['for'];owner=ty.get('resolved_path',{}).get('path')
            if owner:
                for i in impl['items']:owners[str(i)]=owner
        for kind in ('struct','enum','trait'):
            if kind in inner:
                if kind=='enum' and item.get('visibility')=='public':public_variants.update(str(i) for i in inner[kind].get('variants',[]))
                for field in ('fields','items','variants'):
                    value=inner[kind].get(field,[])
                    if isinstance(value,list):
                        for i in value:
                            if i is not None:owners[str(i)]=item.get('name','')
    rows=[]
    for ident,item in items.items():
        if (item.get('visibility')!='public' and ident not in public_variants) or item.get('crate_id')!=0 or not item.get('name') or not item.get('span'):continue
        span=item['span'];path=span['filename'];path=path[path.index('bindings/'):] if 'bindings/' in path else path
        if not path.startswith('bindings/rust/'):continue
        source=(ROOT/path).read_text(encoding='utf-8').splitlines();line=span['begin'][0]
        inner=item['inner'];kind=next(iter(inner));name=item['name']
        if kind in ('impl','use','module'):continue
        if kind in ('function','struct_field','variant'):
            text=' '.join(source[line-1:min(span['end'][0],line+18)])
            text=text.split('{',1)[0].strip() if kind=='function' else source[line-1].strip()
        elif kind in ('struct','enum','trait','type_alias'):
            text=' '.join(source[line-1:min(span['end'][0],line+18)]).split('{',1)[0].strip()
        else:text=kind+' '+name
        owner=owners.get(ident)
        if not owner:
            parts=doc['paths'].get(ident,{}).get('path',['fengqudb']);owner='::'.join(parts[:-1]) or 'fengqudb'
        if crate=='fengqudb_sys':owner='fengqudb_sys'+('::'+owner if owner and owner!='fengqudb_sys' else '')
        rows.append(dict(language='rust',owner=owner,name=name,kind='method' if kind=='function' else 'field' if kind=='struct_field' else 'constant' if kind=='variant' else 'type',signature=text,path=path,line=line))
    return rows


def render_guides(manifest):
    from user_reference import render
    for locale, directory in (('zh-CN', ROOT/'docs/api'), ('en', ROOT/'docs/en/api')):
        directory.mkdir(parents=True, exist_ok=True)
        for lang, inventory in manifest.items():
            (directory/(lang+'.md')).write_text(render(lang,inventory['entries'],locale),encoding='utf-8')
        (directory/'unity.md').write_text(render('unity',manifest['csharp']['entries'],locale),encoding='utf-8')


def main():
    import argparse
    parser=argparse.ArgumentParser(description='Generate Chinese and English API guides from one public API inventory.')
    parser.add_argument('--from-manifest',type=Path,help='Reuse an existing inventory without running compilers')
    args=parser.parse_args()
    if args.from_manifest:
        render_guides(json.loads(args.from_manifest.read_text(encoding='utf-8')))
        return
    all_rows=[]
    for lang in ('csharp','java','go','node'):all_rows.extend(load('api-'+lang+'.json'))
    all_rows.extend(python_rows());all_rows.extend(rust_rows());all_rows.extend(rust_rows('fengqudb_sys'))
    all_rows.extend(compiler_cpp('api-cpp.json','cpp'))
    all_rows.extend(compiler_cpp('api-c.json','c'));all_rows.extend(compiler_cpp('api-c-constants.json','c'))
    # The optional diagnostics adapter is a separate library, not part of the
    # precompiled core SDK. Do not advertise entry points that the SDK lacks.
    all_rows=[r for r in all_rows if not Path(r['path']).name.startswith('fengqudb_diagnostics')]
    out=ROOT/'docs/api';out.mkdir(exist_ok=True)
    manifest={}
    for lang in ('c','cpp','csharp','java','python','go','rust','node'):
        unique={}
        for r in all_rows:
            if r['language']!=lang:continue
            if not r.get('path'):raise ValueError('Missing API location '+str(r))
            # Type bodies contain implementation-private fields in some syntax
            # inventories. Public members have their own entries below.
            if r['kind']=='type' and '{' in r['signature']:r['signature']=r['signature'].split('{')[0].strip()
            r['signature']=re.sub(r'\s+',' ',r['signature']).strip()
            key=(r['owner'],r['name'],r['signature']);unique[key]=r
        rows=sorted(unique.values(),key=lambda r:(r['owner'],r['name'],r['signature']))
        if not rows:raise ValueError('Empty API inventory '+lang)
        manifest[lang]={'count':len(rows),'entries':rows}
    (out/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    render_guides(manifest)
    print({k:v['count'] for k,v in manifest.items()})


if __name__=='__main__':main()
