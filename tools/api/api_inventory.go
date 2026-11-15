// Development-only Go public syntax inventory; uses only the standard library.
package main

import("encoding/json";"go/ast";"go/parser";"go/printer";"go/token";"os";"path/filepath";"strings";"bytes")
type row struct { Language string `json:"language"`; Owner string `json:"owner"`; Name string `json:"name"`; Kind string `json:"kind"`; Signature string `json:"signature"`; Path string `json:"path"`; Line int `json:"line"`; Comment string `json:"comment"` }
func main(){root:=os.Args[1];set:=token.NewFileSet();var rows []row
files,_:=filepath.Glob(filepath.Join(root,"bindings/go/*.go"));for _,path:=range files{if strings.HasSuffix(path,"_test.go"){continue};f,e:=parser.ParseFile(set,path,nil,parser.ParseComments);if e!=nil{panic(e)};rel,_:=filepath.Rel(root,path)
emit:=func(n ast.Node,owner,name,kind string){var b bytes.Buffer;printer.Fprint(&b,set,n);rows=append(rows,row{"go",owner,name,kind,strings.Join(strings.Fields(b.String())," "),filepath.ToSlash(rel),set.Position(n.Pos()).Line,""})}
for _,d:=range f.Decls{switch n:=d.(type){case *ast.FuncDecl:
 if !ast.IsExported(n.Name.Name){continue};owner:="fengqudb";if n.Recv!=nil{var b bytes.Buffer;printer.Fprint(&b,set,n.Recv.List[0].Type);owner+="."+strings.TrimPrefix(b.String(),"*")};copy:=*n;copy.Body=nil;emit(&copy,owner,n.Name.Name,"method")
case *ast.GenDecl:for _,s:=range n.Specs{switch v:=s.(type){case *ast.TypeSpec:if !ast.IsExported(v.Name.Name){continue};emit(v,"fengqudb",v.Name.Name,"type");if st,ok:=v.Type.(*ast.StructType);ok{for _,field:=range st.Fields.List{for _,name:=range field.Names{if ast.IsExported(name.Name){emit(field,"fengqudb."+v.Name.Name,name.Name,"field")}}}}
case *ast.ValueSpec:for _,name:=range v.Names{if ast.IsExported(name.Name){emit(v,"fengqudb",name.Name,"constant")}}}}}}
};json.NewEncoder(os.Stdout).Encode(rows)}
