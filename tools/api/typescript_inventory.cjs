// Development-only compiler inventory. Pass the existing TypeScript module path.
const fs=require('fs'),path=require('path'),ts=require(process.argv[3]);
const root=path.resolve(process.argv[2]),rows=[];
const sourceRoot=path.join(root,'bindings/node/src');
const index=fs.readFileSync(path.join(sourceRoot,'index.ts'),'utf8');
const exported=new Set([...index.matchAll(/export(?: type)?\s*\{([^}]+)\}/gs)].flatMap(m=>m[1].split(',').map(v=>v.trim().split(/\s+as\s+/)[0])));
for(const filename of fs.readdirSync(sourceRoot).filter(p=>p.endsWith('.ts'))){
 const file=path.join(sourceRoot,filename),src=ts.createSourceFile(file,fs.readFileSync(file,'utf8'),ts.ScriptTarget.Latest,true);
 const emit=(n,owner,name,kind)=>{const signature=n.getText(src).split(/\s*\{\s*\n/)[0];rows.push({language:'node',owner,name,kind,signature:signature.replace(/\s+/g,' ').trim(),path:path.relative(root,file).replaceAll('\\','/'),line:src.getLineAndCharacterOfPosition(n.getStart(src)).line+1,comment:''})};
 for(const n of src.statements){
  if(ts.isVariableStatement(n)){for(const v of n.declarationList.declarations){const name=v.name.getText(src);if(exported.has(name))emit(v,'fengqudb',name,'constant');}continue;}
  const name=n.name?.getText(src);if(!name||!exported.has(name))continue;
  if(ts.isClassDeclaration(n)||ts.isInterfaceDeclaration(n)){rows.push({language:'node',owner:'fengqudb',name,kind:'type',signature:(ts.isInterfaceDeclaration(n)?'interface ':'class ')+name+(n.typeParameters?.length?'<'+n.typeParameters.map(p=>p.getText(src)).join(', ')+'>':'')+(n.heritageClauses?.length?' '+n.heritageClauses.map(p=>p.getText(src)).join(' '):''),path:path.relative(root,file).replaceAll('\\','/'),line:src.getLineAndCharacterOfPosition(n.getStart(src)).line+1,comment:''});for(const m of n.members){if(m.modifiers?.some(x=>x.kind===ts.SyntaxKind.PrivateKeyword||x.kind===ts.SyntaxKind.ProtectedKeyword))continue;let mn=m.name?.getText(src)||(ts.isConstructorDeclaration(m)?'constructor':null);if(!mn)continue;
    // Read only the declaration, not a method's implementation.
    const body=m.body;let text=body?src.text.slice(m.getStart(src),body.getStart(src)):m.getText(src);rows.push({language:'node',owner:'fengqudb.'+name,name:mn,kind:ts.isPropertyDeclaration(m)||ts.isPropertySignature(m)?'field':'method',signature:text.replace(/\s+/g,' ').trim(),path:path.relative(root,file).replaceAll('\\','/'),line:src.getLineAndCharacterOfPosition(m.getStart(src)).line+1,comment:''});}}
  else if(ts.isFunctionDeclaration(n)){const text=n.body?src.text.slice(n.getStart(src),n.body.getStart(src)):n.getText(src);rows.push({language:'node',owner:'fengqudb',name,kind:'method',signature:text.replace(/\s+/g,' ').trim(),path:path.relative(root,file).replaceAll('\\','/'),line:src.getLineAndCharacterOfPosition(n.getStart(src)).line+1,comment:''});}
  else if(ts.isEnumDeclaration(n)){emit(n,'fengqudb',name,'type');for(const m of n.members)emit(m,'fengqudb.'+name,m.name.getText(src),'constant');}
  else emit(n,'fengqudb',name,'type');
 }
}
console.log(JSON.stringify(rows));
