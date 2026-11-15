# Native FengquDB model compiler

Build the host executable alone with `cmake -S tools/codegen -B build/codegen`
and `cmake --build build/codegen --config Release`.

```sh
fqdb-codegen generate --schema examples/models/student.fqmodel.json --output Generated
fqdb-codegen generate --schema examples/models/student.fqmodel.json --output Generated --lang csharp,java,python
```

Templates are embedded; the tool does not load a database, language SDK,
protobuf runtime or .NET. See [the model guide](../../docs/MODEL_CODEGEN.md)
for supported types, evolution rules, buffer ownership and build hooks.

The bundled JSON parser is used only by this development tool, under the
existing third_party/json MIT license. No parser or generator code is linked
into the database runtime.
