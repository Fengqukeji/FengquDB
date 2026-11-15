using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

// Development-only documentation inventory. Uses the installed compiler API;
// none of this tool or its dependencies is part of the client distribution.
var root = Path.GetFullPath(args[0]);
var rows = new List<object>();
foreach (var path in Directory.GetFiles(Path.Combine(root,"bindings/csharp"),"*.cs")) {
    var tree = CSharpSyntaxTree.ParseText(File.ReadAllText(path));
    var syntax = tree.GetRoot();
    foreach (var node in syntax.DescendantNodes().OfType<MemberDeclarationSyntax>()) {
        var types = node.Ancestors().OfType<BaseTypeDeclarationSyntax>().Reverse().ToArray();
        if (types.Any(t => !t.Modifiers.Any(SyntaxKind.PublicKeyword))) continue;
        string signature=null,name=null,kind=null;
        var modifiers=node switch {BaseTypeDeclarationSyntax t=>t.Modifiers,BaseMethodDeclarationSyntax m=>m.Modifiers,BasePropertyDeclarationSyntax p=>p.Modifiers,BaseFieldDeclarationSyntax f=>f.Modifiers,DelegateDeclarationSyntax d=>d.Modifiers,_=>default};
        bool isPublic = modifiers.Any(SyntaxKind.PublicKeyword) || node.Parent is InterfaceDeclarationSyntax;
        if (node is EnumMemberDeclarationSyntax em) {isPublic=true;signature=em.ToString();name=em.Identifier.Text;kind="constant";}
        else if (node is BaseTypeDeclarationSyntax type && isPublic) {name=type.Identifier.Text;kind="type";signature="public "+(type is EnumDeclarationSyntax ? "enum" : type is StructDeclarationSyntax ? "struct" : type is InterfaceDeclarationSyntax ? "interface" : type is RecordDeclarationSyntax ? "record" : "class")+" "+name+((type as TypeDeclarationSyntax)?.TypeParameterList?.ToString() ?? "");}
        else if (node is MethodDeclarationSyntax m && isPublic) {name=m.Identifier.Text;kind="method";signature=m.WithBody(null).WithExpressionBody(null).WithSemicolonToken(default).ToString();}
        else if (node is ConstructorDeclarationSyntax c && isPublic) {name=c.Identifier.Text;kind="constructor";signature=c.WithBody(null).WithExpressionBody(null).WithSemicolonToken(default).ToString();}
        else if (node is OperatorDeclarationSyntax op && isPublic) {name="operator "+op.OperatorToken.Text;kind="method";signature=op.WithBody(null).WithExpressionBody(null).WithSemicolonToken(default).ToString();}
        else if (node is PropertyDeclarationSyntax prop && isPublic) {name=prop.Identifier.Text;kind="property";signature=prop.Type+" "+name+" { "+string.Join("; ",prop.AccessorList?.Accessors.Select(a=>a.Keyword.Text) ?? new[]{"get"})+"; }";}
        else if (node is IndexerDeclarationSyntax ix && isPublic) {name="this";kind="property";signature=ix.Type+" this"+ix.ParameterList;}
        else if (node is FieldDeclarationSyntax f && isPublic) {name=string.Join(", ",f.Declaration.Variables.Select(v=>v.Identifier.Text));kind="field";signature=f.Declaration.ToString();}
        else if (node is DelegateDeclarationSyntax d && isPublic) {name=d.Identifier.Text;kind="type";signature=d.ToString();}
        if (name==null) continue;
        var ns=string.Join(".",node.Ancestors().OfType<BaseNamespaceDeclarationSyntax>().Reverse().Select(n=>n.Name.ToString()));
        var owner=string.Join(".",new[]{ns}.Concat(types.Select(t=>t.Identifier.Text)).Where(s=>s.Length!=0));
        var comment=string.Join(" ",node.GetLeadingTrivia().Select(t=>t.ToString()).Where(t=>t.TrimStart().StartsWith("//")));
        rows.Add(new {language="csharp",owner,name,kind,signature=Regex.Replace(signature,@"\s+"," ").Trim(),path=Path.GetRelativePath(root,path).Replace('\\','/'),line=tree.GetLineSpan(node.Span).StartLinePosition.Line+1,comment});
    }
}
Console.WriteLine(JsonSerializer.Serialize(rows));
