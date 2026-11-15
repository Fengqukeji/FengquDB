using System;
using System.IO;
using System.Linq;
using FengquDB.Entities;
using FengquDB.Generators;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
    .Append(typeof(FqEntityAttribute).Assembly.Location).Distinct()
    .Select(path => MetadataReference.CreateFromFile(path)).ToArray();
var parse = new CSharpParseOptions(LanguageVersion.CSharp9);
void Run(string source, bool valid) {
    var compilation = CSharpCompilation.Create("Entities", new[] { CSharpSyntaxTree.ParseText(source, parse) },
        references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
    GeneratorDriver driver = CSharpGeneratorDriver.Create(new[] { new EntityGenerator().AsSourceGenerator() }, parseOptions: parse);
    driver = driver.RunGeneratorsAndUpdateCompilation(compilation, out var output, out var errors);
    var generated = driver.GetRunResult();
    if (generated.Results.Any(r => r.Exception != null)) throw new Exception("Generator crashed");
    if (valid) {
        var diagnostics = output.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error).ToArray();
        if (errors.Any(d => d.Severity == DiagnosticSeverity.Error) || diagnostics.Length != 0)
            throw new Exception(string.Join("\n", errors.Concat(diagnostics)));
        using var stream = new MemoryStream();
        if (!output.Emit(stream).Success) throw new Exception("Generated assembly could not be emitted");
    } else if (!errors.Any(d => d.Id == "FQDB001")) throw new Exception("Invalid schema silently accepted");
}
const string header = "using FengquDB.Entities; ";
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int Id; [FqField(2)] public object Bad; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int Id; [FqField(1)] public int Duplicate; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(19000)] public int Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public readonly int Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int Id {get; init;} }", false);
Run(header + "[FqEntity(\"a/\")] public class A { [FqKey,FqField(1)] public int Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqField(1)] public int Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A<T> { [FqKey,FqField(1)] public int Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int? Id; }", false);
Run(header + "[FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int Id; public void EncodeTo() {} }", false);
Run(header + "[FqEntity(\"a/\")] public partial record A { [FqKey,FqField(1)] public int Id; }", false);
Run(header + "namespace One { [FqEntity(\"a/\")] public partial class A { [FqKey,FqField(1)] public int Id; } } " +
    "namespace Two { [FqEntity(\"b/\")] public partial class A { [FqKey,FqField(1)] public string Id=\"\"; [FqField(2)] public int? Score; } }", true);
Run(header + "[FqEntity(\"quoted\\\"/\")] public sealed partial class @event { [FqKey,FqField(1)] public long @class; } " +
    "public sealed partial class @event { [FqField(2)] public string? @namespace; }", true);
Run(header + "namespace Shadow { class Span<T> {} class ReadOnlySpan<T> {} class Func<T,U> {} " +
    "[FqEntity(\"shadow/\")] public partial class A { [FqKey,FqField(1)] public int Id; } }", true);
Console.WriteLine("PASS source generator schema diagnostics, same-name namespaces, partial declarations and escaped identifiers");

// Reuse one driver across edits, as an IDE does, including a changed member in
// a different partial declaration from the FqEntity attribute.
var entityTree = CSharpSyntaxTree.ParseText(header +
    "[FqEntity(\"incremental/\")] public partial class Model { [FqKey,FqField(1)] public int Id; }", parse, "Model.cs");
var fieldsTree = CSharpSyntaxTree.ParseText("public partial class Model { [FengquDB.Entities.FqField(2)] public int Score; }", parse, "Fields.cs");
var current = CSharpCompilation.Create("Incremental", new[] { entityTree, fieldsTree }, references,
    new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
GeneratorDriver incremental = CSharpGeneratorDriver.Create(new[] { new EntityGenerator().AsSourceGenerator() }, parseOptions: parse);
incremental = incremental.RunGenerators(current);
string original = incremental.GetRunResult().Results.Single().GeneratedSources.Single().SourceText.ToString();
var changedFields = CSharpSyntaxTree.ParseText("public partial class Model { [FengquDB.Entities.FqField(3)] public long Bonus; }", parse, "Fields.cs");
current = current.ReplaceSyntaxTree(fieldsTree, changedFields);
incremental = incremental.RunGenerators(current);
string changed = incremental.GetRunResult().Results.Single().GeneratedSources.Single().SourceText.ToString();
if (original == changed || !changed.Contains("ReadBonus") || changed.Contains("ReadScore"))
    throw new Exception("Incremental generation did not follow a partial class edit");

var manualParse = parse.WithPreprocessorSymbols(EntityGenerator.ManualSymbol);
var manualTree = CSharpSyntaxTree.ParseText(changed, manualParse, "FengquDB.Entities.g.cs");
var manualCompilation = CSharpCompilation.Create("Manual", new[] {
    entityTree.WithRootAndOptions(entityTree.GetRoot(), manualParse),
    changedFields.WithRootAndOptions(changedFields.GetRoot(), manualParse), manualTree
}, references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
GeneratorDriver manualDriver = CSharpGeneratorDriver.Create(new[] { new EntityGenerator().AsSourceGenerator() }, parseOptions: manualParse);
manualDriver = manualDriver.RunGeneratorsAndUpdateCompilation(manualCompilation, out var manualOutput, out var manualErrors);
if (manualErrors.Any(d => d.Severity == DiagnosticSeverity.Error) || manualDriver.GetRunResult().GeneratedTrees.Length != 0 ||
    manualOutput.GetDiagnostics().Any(d => d.Severity == DiagnosticSeverity.Error))
    throw new Exception("Matching manual output conflicted with the automatic generator");
var staleFields = CSharpSyntaxTree.ParseText("public partial class Model { [FengquDB.Entities.FqField(4)] public long Bonus; }", manualParse, "Fields.cs");
manualCompilation = manualCompilation.ReplaceSyntaxTree(manualCompilation.SyntaxTrees.Single(t => t.FilePath == "Fields.cs"), staleFields);
manualDriver = manualDriver.RunGeneratorsAndUpdateCompilation(manualCompilation, out _, out manualErrors);
if (!manualErrors.Any(d => d.Id == "FQDB002")) throw new Exception("Stale manual serialization was accepted");
var unmarked = CSharpSyntaxTree.ParseText("public partial class Model { public int Id; }", manualParse, "Model.cs");
manualCompilation = manualCompilation.ReplaceSyntaxTree(manualCompilation.SyntaxTrees.Single(t => t.FilePath == "Model.cs"), unmarked);
manualDriver = manualDriver.RunGeneratorsAndUpdateCompilation(manualCompilation, out _, out manualErrors);
if (!manualErrors.Any(d => d.Id == "FQDB002")) throw new Exception("Orphan manual serialization was accepted");
Console.WriteLine("PASS automatic incremental edits, manual coexistence and stale-schema diagnostics");
