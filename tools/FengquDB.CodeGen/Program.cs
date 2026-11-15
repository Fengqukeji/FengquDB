using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using FengquDB.Generators;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace FengquDB.CodeGen;

// Build-time entry point. Both input modes run the production generator afresh.
internal static class Program {
    internal static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
    internal static async Task<int> Main(string[] args) {
        try {
            if (args.Length == 0 || args.Contains("--help")) { Console.WriteLine(Options.Help); return 0; }
            if (args is ["--version"]) { Console.WriteLine("1.0.0"); return 0; }
            var options = Options.Parse(args);
            using var cancellation = new CancellationTokenSource();
            Console.CancelKeyPress += (_, e) => { e.Cancel = true; cancellation.Cancel(); };
            var input = options.Project != null
                ? await ProjectInput.LoadAsync(options, cancellation.Token)
                : await SourceInput.LoadAsync(options, cancellation.Token);
            var compilation = input.Compilation;
            var parse = compilation.SyntaxTrees.FirstOrDefault()?.Options as CSharpParseOptions
                ?? new CSharpParseOptions(LanguageVersion.Latest).WithPreprocessorSymbols(input.Defines);
            GeneratorDriver driver = CSharpGeneratorDriver.Create(
                new[] { new EntityGenerator().AsSourceGenerator() }, parseOptions: parse);
            driver = driver.RunGeneratorsAndUpdateCompilation(compilation, out var updated, out var diagnostics, cancellation.Token);
            var result = driver.GetRunResult();
            var failures = result.Results.Where(r => r.Exception != null).Select(r => r.Exception!.ToString()).ToArray();
            var errors = diagnostics.Concat(updated.GetDiagnostics(cancellation.Token))
                .Where(d => d.Severity == DiagnosticSeverity.Error).Select(d => d.ToString()).Distinct().ToArray();
            if (errors.Length != 0 || failures.Length != 0)
                throw new InvalidOperationException(string.Join(Environment.NewLine, failures.Concat(errors)));
            var generated = result.Results.SelectMany(r => r.GeneratedSources).OrderBy(s => s.HintName, StringComparer.Ordinal).ToArray();
            string content = OutputFile.Render(generated.Select(s => s.SourceText.ToString()));
            string path = Path.Combine(options.Output, OutputFile.Name);
            bool changed = await OutputFile.PublishAsync(path, content, options.Check, cancellation.Token);
            Console.WriteLine(JsonSerializer.Serialize(new {
                ok = !options.Check || !changed, status = options.Check && changed ? "out_of_date" : "ok",
                changed, entities = generated.Length, file = path,
                sha256 = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(content))),
                manualSymbol = EntityGenerator.ManualSymbol, warnings = input.Warnings
            }, Json));
            return options.Check && changed ? 2 : 0;
        } catch (Exception error) {
            Console.Error.WriteLine(error.Message);
            Console.WriteLine(JsonSerializer.Serialize(new { ok = false, error = error.Message }, Json));
            return 1;
        }
    }
}

internal sealed class Options {
    internal string? Project;
    internal string Output = "";
    internal string Configuration = "Debug";
    internal string? Framework;
    internal bool Check, NoRestore;
    internal readonly List<string> Sources = new(), References = new(), Defines = new();
    internal const string Help = """
        fqdb-csharp-codegen generate --project App.csproj --output Generated [--configuration Release] [--framework net10.0] [--no-restore] [--check]
        fqdb-csharp-codegen generate --source Models --output Generated [--source Other.cs] [--reference UnityEngine.CoreModule.dll] [--define UNITY_EDITOR] [--check]

        Every invocation runs the current entity generator from scratch.
        Output is one atomic FengquDB.Entities.g.cs file, inactive by default.
        Define FQDB_MANUAL_ENTITIES to compile this file as the manual fallback.
        Exit codes are 0 success, 1 failure, 2 stale/missing output with --check.
        Standard output is JSON. Diagnostics are also written to standard error.
        """;
    internal static Options Parse(string[] args) {
        if (args[0] != "generate") throw new ArgumentException("Expected generate. Use --help for usage.");
        var options = new Options();
        for (int i = 1; i < args.Length; ++i) {
            string Next() => ++i < args.Length ? args[i] : throw new ArgumentException("Missing value for " + args[i - 1]);
            switch (args[i]) {
            case "--project": options.Project = Path.GetFullPath(Next()); break;
            case "--output": options.Output = Path.GetFullPath(Next()); break;
            case "--source": options.Sources.Add(Path.GetFullPath(Next())); break;
            case "--reference": options.References.Add(Path.GetFullPath(Next())); break;
            case "--define": options.Defines.AddRange(Next().Split(new[] { ';', ',' }, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)); break;
            case "--configuration": options.Configuration = Next(); break;
            case "--framework": options.Framework = Next(); break;
            case "--check": options.Check = true; break;
            case "--no-restore": options.NoRestore = true; break;
            default: throw new ArgumentException("Unknown option " + args[i]);
            }
        }
        if (options.Output.Length == 0 || (options.Project == null) == (options.Sources.Count == 0))
            throw new ArgumentException("Specify --output and exactly one input mode, --project or --source.");
        if (options.Project != null && (!File.Exists(options.Project) || !options.Project.EndsWith(".csproj", StringComparison.OrdinalIgnoreCase)))
            throw new ArgumentException("Project must be an existing C# project.");
        if (options.Project != null && options.References.Count != 0)
            throw new ArgumentException("Use project references in --project mode. --reference is for --source mode.");
        return options;
    }
}
