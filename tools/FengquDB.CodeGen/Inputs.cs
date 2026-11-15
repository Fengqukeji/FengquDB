using System.Diagnostics;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using FengquDB.Entities;
using FengquDB.Generators;
using Microsoft.Build.Locator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.MSBuild;

namespace FengquDB.CodeGen;

internal sealed record GeneratorInput(CSharpCompilation Compilation, string[] Defines, string[] Warnings);

internal static class SourceInput {
    static readonly HashSet<string> Excluded = new(StringComparer.OrdinalIgnoreCase) {
        "bin", "obj", ".git", "Library", "Temp", "Logs", "UserSettings"
    };
    internal static async Task<GeneratorInput> LoadAsync(Options options, CancellationToken token) {
        var files = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        foreach (string path in options.Sources) {
            if (File.Exists(path)) files.Add(path);
            else if (Directory.Exists(path)) Find(path, options.Output, files);
            else throw new FileNotFoundException("Source input does not exist", path);
        }
        string[] defines = options.Defines.Append(EntityGenerator.ForceSymbol).Distinct().ToArray();
        var parse = new CSharpParseOptions(LanguageVersion.Latest).WithPreprocessorSymbols(defines);
        var trees = new List<SyntaxTree>();
        foreach (string file in files.OrderBy(f => f, StringComparer.Ordinal))
            trees.Add(CSharpSyntaxTree.ParseText(await File.ReadAllTextAsync(file, token), parse, file, cancellationToken: token));
        var assemblies = ((string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")
            ?? throw new InvalidOperationException("Cannot find the .NET reference assemblies")).Split(Path.PathSeparator)
            .Append(typeof(FqEntityAttribute).Assembly.Location).Concat(options.References).Distinct();
        var references = assemblies.Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("FengquDB.CodeGen.Input", trees, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true,
                                         nullableContextOptions: NullableContextOptions.Enable));
        return new GeneratorInput(compilation, defines, Array.Empty<string>());
    }
    static void Find(string directory, string output, HashSet<string> files) {
        var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        foreach (string file in Directory.EnumerateFiles(directory, "*.cs"))
            if (!string.Equals(Path.GetFullPath(file), Path.Combine(output, OutputFile.Name), comparison)) files.Add(file);
        foreach (string child in Directory.EnumerateDirectories(directory)) {
            if (Excluded.Contains(Path.GetFileName(child)) ||
                (File.GetAttributes(child) & FileAttributes.ReparsePoint) != 0 ||
                string.Equals(Path.GetFullPath(child), output, comparison)) continue;
            Find(child, output, files);
        }
    }
}

internal static class ProjectInput {
    internal static Task<GeneratorInput> LoadAsync(Options options, CancellationToken token) {
        if (!MSBuildLocator.IsRegistered) MSBuildLocator.RegisterDefaults();
        return LoadRegisteredAsync(options, token);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static async Task<GeneratorInput> LoadRegisteredAsync(Options options, CancellationToken token) {
        if (!options.NoRestore) {
            var start = new ProcessStartInfo("dotnet") { RedirectStandardOutput = true, RedirectStandardError = true };
            // A short-lived generator must not depend on a pre-existing build
            // server or inherit its lifecycle from the host application's build.
            foreach (string arg in new[] { "restore", options.Project!, "--disable-build-servers", "--nologo", "--verbosity", "quiet" }) start.ArgumentList.Add(arg);
            if (options.Framework != null) start.ArgumentList.Add("-p:TargetFramework=" + options.Framework);
            using var process = Process.Start(start) ?? throw new InvalidOperationException("Could not start dotnet restore");
            var stdout = process.StandardOutput.ReadToEndAsync(token);
            var stderr = process.StandardError.ReadToEndAsync(token);
            try { await process.WaitForExitAsync(token); }
            finally { if (!process.HasExited) process.Kill(entireProcessTree: true); }
            string[] messages = await Task.WhenAll(stdout, stderr);
            if (process.ExitCode != 0) throw new InvalidOperationException("Project restore failed\n" + string.Join("\n", messages));
        }
        var properties = new Dictionary<string, string> { ["Configuration"] = options.Configuration };
        if (options.Framework != null) properties["TargetFramework"] = options.Framework;
        using var workspace = MSBuildWorkspace.Create(properties);
        var warnings = new ConcurrentQueue<string>();
        var failures = new ConcurrentQueue<string>();
        using var registration = workspace.RegisterWorkspaceFailedHandler(e => {
            if (e.Diagnostic.Kind == WorkspaceDiagnosticKind.Failure) failures.Enqueue(e.Diagnostic.Message);
            else warnings.Enqueue(e.Diagnostic.Message);
        });
        var project = await workspace.OpenProjectAsync(options.Project!, cancellationToken: token);
        if (project.ParseOptions is not CSharpParseOptions parse)
            throw new InvalidOperationException("Project must use the C# compiler");
        string[] defines = parse.PreprocessorSymbolNames.Concat(options.Defines).Append(EntityGenerator.ForceSymbol).Distinct().ToArray();
        // Replace the project's cached analyzer with this CLI's current generator.
        project = project.WithAnalyzerReferences(project.AnalyzerReferences.Where(r => !OwnAnalyzer(r.FullPath)))
            .WithParseOptions(parse.WithPreprocessorSymbols(defines));
        var compilation = await project.GetCompilationAsync(token) as CSharpCompilation
            ?? throw new InvalidOperationException("Could not load the project compilation");
        if (!failures.IsEmpty) throw new InvalidOperationException(string.Join(Environment.NewLine, failures));
        return new GeneratorInput(compilation, defines, warnings.ToArray());
    }
    static bool OwnAnalyzer(string? path) {
        if (path == null) return false;
        string name = typeof(EntityGenerator).Assembly.GetName().Name!;
        if (string.Equals(Path.GetFileNameWithoutExtension(path), name, StringComparison.OrdinalIgnoreCase)) return true;
        return File.Exists(path) && string.Equals(AssemblyName.GetAssemblyName(path).Name, name, StringComparison.OrdinalIgnoreCase);
    }
}
