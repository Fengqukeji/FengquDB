using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using Xunit;

// Seeds a fixture with the Python query regression, then runs the C# query smoke
// (raw StoreQuery and the typed Q builder) against the same native library.
// The native library comes from FQDB_LIBRARY or the newest fengqudb.dll / libfengqudb.so under build/.
public class QueryTests {
    static string Root() {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null && !File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
            dir = dir.Parent;
        return dir?.FullName ?? throw new InvalidOperationException("repository root not found");
    }
    static string Library(string root) {
        var explicitPath = Environment.GetEnvironmentVariable("FQDB_LIBRARY");
        if (!string.IsNullOrEmpty(explicitPath)) return explicitPath;
        var names = new[] { "fengqudb.dll", "libfengqudb.so", "libfengqudb.dylib" };
        return Directory.EnumerateFiles(Path.Combine(root, "build"), "*", SearchOption.AllDirectories)
            .Where(p => names.Contains(Path.GetFileName(p)))
            .OrderByDescending(File.GetLastWriteTimeUtc)
            .First();
    }

    [Fact]
    public void QueryBuildersAgainstNativeCore() {
        var root = Root();
        var library = Library(root);
        var fixture = Path.Combine(Path.GetTempPath(), "fqdb-cs-query-" + Guid.NewGuid().ToString("N"));
        try {
            var python = Environment.GetEnvironmentVariable("PYTHON") ?? (OperatingSystem.IsWindows() ? "python" : "python3");
            var seed = Process.Start(new ProcessStartInfo(python) {
                ArgumentList = { Path.Combine(root, "tests", "store_select_bindings.py"), fixture, library },
                UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true })!;
            var output = seed.StandardOutput.ReadToEnd() + seed.StandardError.ReadToEnd();
            seed.WaitForExit();
            Assert.True(seed.ExitCode == 0, output);
            var main = typeof(QueryTests).Assembly.GetType("SelectSmoke")!
                .GetMethod("Main", BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public)!;
            try {
                main.Invoke(null, new object[] { new[] { Path.Combine(fixture, "main"), library } });
            } catch (TargetInvocationException e) when (e.InnerException != null) {
                throw e.InnerException;
            }
        } finally {
            try { Directory.Delete(fixture, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        }
    }
}
