import java.io.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import javax.tools.*;
import javax.lang.model.element.Modifier;
import com.sun.source.tree.*;
import com.sun.source.util.*;

/** Development-only public Java syntax inventory, using the installed javac. */
public class ApiInventory {
    static String json(String s) {
        StringBuilder b=new StringBuilder("\"");
        for(char c:s.toCharArray()){switch(c){case '\\':b.append("\\\\");break;case '"':b.append("\\\"");break;case '\n':b.append("\\n");break;case '\r':b.append("\\r");break;case '\t':b.append("\\t");break;default:if(c<32)b.append(String.format("\\u%04x",(int)c));else b.append(c);}}
        return b.append('"').toString();
    }
    static String generics(List<? extends TypeParameterTree> params) {
        return params.isEmpty()?"":"<"+String.join(", ",params.stream().map(Object::toString).toArray(String[]::new))+">";
    }
    public static void main(String[] args)throws Exception {
        Path root=Path.of(args[0]).toAbsolutePath();List<Path> files=new ArrayList<>();
        try(var stream=Files.walk(root.resolve("bindings/java/io/fengqudb"))){stream.filter(p->p.toString().endsWith(".java")).forEach(files::add);}
        JavaCompiler compiler=ToolProvider.getSystemJavaCompiler();
        DiagnosticCollector<JavaFileObject> diagnostics=new DiagnosticCollector<>();
        try(StandardJavaFileManager fm=compiler.getStandardFileManager(diagnostics,null,StandardCharsets.UTF_8)){
            JavacTask task=(JavacTask)compiler.getTask(null,fm,diagnostics,List.of("-proc:none","-encoding","UTF-8"),null,fm.getJavaFileObjectsFromPaths(files));
            Iterable<? extends CompilationUnitTree> units=task.parse();Trees trees=Trees.instance(task);List<String> output=new ArrayList<>();
            for(CompilationUnitTree unit:units){
                new TreeScanner<Void,Void>(){
                    final ArrayDeque<String> owners=new ArrayDeque<>();boolean visible=true, inInterface=false;
                    void row(Tree n,String name,String kind,String signature){String owner=unit.getPackageName().toString()+(owners.isEmpty()?"":"."+String.join(".",owners));long line=unit.getLineMap().getLineNumber(trees.getSourcePositions().getStartPosition(unit,n));String path=root.relativize(Path.of(unit.getSourceFile().toUri())).toString().replace('\\','/');output.add("{\"language\":\"java\",\"owner\":"+json(owner)+",\"name\":"+json(name)+",\"kind\":"+json(kind)+",\"signature\":"+json(signature.replaceAll("\\s+"," ").trim())+",\"path\":"+json(path)+",\"line\":"+line+",\"comment\":\"\"}");}
                    public Void visitClass(ClassTree c,Void x){boolean old=visible,oldInterface=inInterface;visible=visible&&(c.getModifiers().getFlags().contains(Modifier.PUBLIC)||inInterface);inInterface=c.getKind()==Tree.Kind.INTERFACE||c.getKind()==Tree.Kind.ANNOTATION_TYPE;String signature="public "+c.getKind().toString().toLowerCase()+" "+c.getSimpleName()+generics(c.getTypeParameters());if(c.getExtendsClause()!=null)signature+=" extends "+c.getExtendsClause();if(!c.getImplementsClause().isEmpty())signature+=(inInterface?" extends ":" implements ")+String.join(", ",c.getImplementsClause().stream().map(Object::toString).toArray(String[]::new));if(visible)row(c,c.getSimpleName().toString(),"type",signature);owners.addLast(c.getSimpleName().toString());super.visitClass(c,x);owners.removeLast();visible=old;inInterface=oldInterface;return null;}
                    public Void visitMethod(MethodTree m,Void x){if(visible&&(m.getModifiers().getFlags().contains(Modifier.PUBLIC)||inInterface)){String name=m.getName().toString();if(name.equals("<init>"))name=owners.getLast();String s=m.getModifiers()+" "+generics(m.getTypeParameters())+" "+(m.getReturnType()==null?"":m.getReturnType()+" ")+name+"("+String.join(", ",m.getParameters().stream().map(Object::toString).toArray(String[]::new))+")";if(!m.getThrows().isEmpty())s+=" throws "+String.join(", ",m.getThrows().stream().map(Object::toString).toArray(String[]::new));row(m,name,m.getReturnType()==null?"constructor":"method",s);}return null;}
                    public Void visitVariable(VariableTree v,Void x){if(visible&&(v.getModifiers().getFlags().contains(Modifier.PUBLIC)||inInterface))row(v,v.getName().toString(),"field",v.getModifiers()+" "+v.getType()+" "+v.getName());return null;}
                }.scan(unit,null);
            }
            if(diagnostics.getDiagnostics().stream().anyMatch(d->d.getKind()==Diagnostic.Kind.ERROR))throw new IllegalStateException(diagnostics.getDiagnostics().toString());
            System.out.println("["+String.join(",",output)+"]");
        }
    }
}
