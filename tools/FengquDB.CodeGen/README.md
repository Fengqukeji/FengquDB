# FengquDB optional C# semantic adapter

The cross-language CLI is the standalone native `fqdb-codegen` in `tools/codegen`.
This optional tool reads existing attributed C# classes using Roslyn. It is not
required for schema generation or other language projects.

Force-generate the production C# entity serializers from a project or source files.

```sh
fqdb-csharp-codegen generate --project App.csproj --output Generated
fqdb-csharp-codegen generate --source Models --output Generated
fqdb-csharp-codegen generate --source Models --output Generated --check
```

Every invocation computes fresh output. One deterministic `FengquDB.Entities.g.cs`
file is atomically replaced. Existing unrelated source files are protected.

The output is a manual fallback. Define `FQDB_MANUAL_ENTITIES` in the business
assembly to compile it. The automatic analyzer then checks its schema stamp and
reports `FQDB002` when the entity changes. Without that symbol, normal automatic
generation continues and the fallback file stays inactive.

The command writes JSON to standard output. Exit codes are 0 for success, 1 for
failure, and 2 for missing or stale output with `--check`. Source mode supports
repeated `--source`, `--reference`, and `--define` arguments. Project mode uses
the project's references and compiler settings, accepts `--configuration` and
`--framework`, and restores dependencies unless `--no-restore` is supplied.

The tool requires .NET 10. Project mode also requires an installed .NET SDK.
The generator and CLI run during development and builds.
