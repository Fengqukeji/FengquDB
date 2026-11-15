using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace FengquDB.Generators {
// Generates direct scalar protobuf code and typed columns, with diagnostics
// instead of silently discarding unsupported or ambiguous persistent fields.
[Generator]
public sealed class EntityGenerator : IIncrementalGenerator {
    public const string ManualSymbol = "FQDB_MANUAL_ENTITIES";
    public const string ForceSymbol = "FQDB_FORCE_ENTITY_GENERATION";
    const string EntityAttribute = "FengquDB.Entities.FqEntityAttribute";
    const string FieldAttribute = "FengquDB.Entities.FqFieldAttribute";
    const string KeyAttribute = "FengquDB.Entities.FqKeyAttribute";
    const string Wire = "global::FengquDB.Entities.FqWire";
    static readonly DiagnosticDescriptor Invalid = new DiagnosticDescriptor(
        "FQDB001", "Invalid FengquDB entity", "{0}", "FengquDB", DiagnosticSeverity.Error, true);
    static readonly DiagnosticDescriptor Stale = new DiagnosticDescriptor(
        "FQDB002", "Stale manual entity code",
        "Manual serialization for {0} is missing or stale. Run fqdb-codegen generate again, or remove FQDB_MANUAL_ENTITIES to use automatic generation.",
        "FengquDB", DiagnosticSeverity.Error, true);
    sealed class Member {
        internal string Name = "", Type = "", ValueType = "", Kind = "";
        internal int Number;
        internal bool Optional, NullableReference, Key;
        internal int WireType => Kind == "double" ? 1 : Kind == "single" ? 5
            : Kind == "string" || Kind == "bytes" ? 2 : 0;
        internal uint Tag => ((uint)Number << 3) | (uint)WireType;
        internal bool Reference => Kind == "string" || Kind == "bytes";
    }
    sealed class Model {
        internal string Namespace = "", Name = "", Prefix = "", FullName = "";
        internal string? GeneratedSchema;
        internal bool Manual;
        internal Location? Location;
        internal readonly List<Member> Members = new List<Member>();
        internal readonly List<Diagnostic> Errors = new List<Diagnostic>();
    }
    public void Initialize(IncrementalGeneratorInitializationContext context) {
        var models = context.SyntaxProvider.CreateSyntaxProvider(
            static (node, _) => node is TypeDeclarationSyntax t &&
                (t is ClassDeclarationSyntax || t is RecordDeclarationSyntax) && t.AttributeLists.Count > 0,
            static (ctx, ct) => Build(ctx, ct)).Where(static model => model != null);
        context.RegisterSourceOutput(models.Collect(), static (output, collected) => {
            var emitted = new HashSet<string>(StringComparer.Ordinal);
            foreach (var item in collected) {
                var model = item!;
                if (!emitted.Add(model.FullName)) continue;
                foreach (var error in model.Errors) output.ReportDiagnostic(error);
                if (model.Errors.Count != 0) continue;
                if (model.Manual) {
                    if (model.GeneratedSchema != Fingerprint(model))
                        output.ReportDiagnostic(Diagnostic.Create(Stale, model.Location, model.FullName));
                    continue;
                }
                string hint;
                using (var hash = System.Security.Cryptography.SHA256.Create())
                    hint = BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(model.FullName))).Replace("-", "");
                output.AddSource(hint + ".FqEntity.g.cs", SourceText.From(Emit(model), Encoding.UTF8));
            }
        });
    }
    static AttributeData? Attribute(ISymbol symbol, string name) =>
        symbol.GetAttributes().FirstOrDefault(a => a.AttributeClass?.ToDisplayString() == name);
    static string? Kind(ITypeSymbol type) {
        switch (type.SpecialType) {
        case SpecialType.System_Byte: case SpecialType.System_UInt16:
        case SpecialType.System_UInt32: case SpecialType.System_UInt64: return "unsigned";
        case SpecialType.System_SByte: case SpecialType.System_Int16:
        case SpecialType.System_Int32: case SpecialType.System_Int64: return "signed";
        case SpecialType.System_Boolean: return "bool";
        case SpecialType.System_Single: return "single";
        case SpecialType.System_Double: return "double";
        case SpecialType.System_String: return "string";
        }
        return type is IArrayTypeSymbol array && array.Rank == 1 &&
            array.ElementType.SpecialType == SpecialType.System_Byte ? "bytes" : null;
    }
    static Model? Build(GeneratorSyntaxContext context, System.Threading.CancellationToken token) {
        var declaration = (TypeDeclarationSyntax)context.Node;
        if (!(context.SemanticModel.GetDeclaredSymbol(declaration, token) is INamedTypeSymbol type)) return null;
        var symbols = ((CSharpParseOptions)declaration.SyntaxTree.Options).PreprocessorSymbolNames;
        bool manual = symbols.Contains(ManualSymbol) && !symbols.Contains(ForceSymbol);
        var stamp = Attribute(type, "FengquDB.Entities.FqGeneratedEntityAttribute");
        var entity = Attribute(type, EntityAttribute);
        if (entity == null) {
            if (!manual || stamp == null) return null;
            var orphan = new Model { FullName = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) };
            orphan.Errors.Add(Diagnostic.Create(Stale, type.Locations.FirstOrDefault(), orphan.FullName));
            return orphan;
        }
        var model = new Model {
            Namespace = type.ContainingNamespace.IsGlobalNamespace ? "" : type.ContainingNamespace.ToDisplayString(),
            Name = type.Name, FullName = type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
            Prefix = entity.ConstructorArguments.Length == 1 ? entity.ConstructorArguments[0].Value as string ?? "" : "",
            Location = type.Locations.FirstOrDefault()
        };
        model.Manual = manual;
        model.GeneratedSchema = stamp?.ConstructorArguments.FirstOrDefault().Value as string;
        void Error(ISymbol symbol, string message) => model.Errors.Add(Diagnostic.Create(
            Invalid, symbol.Locations.FirstOrDefault(), message));
        if (type.IsAbstract || type.IsStatic || type.IsRecord || type.Arity != 0 || type.ContainingType != null ||
            !type.DeclaringSyntaxReferences.All(r => r.GetSyntax(token) is ClassDeclarationSyntax c &&
                                               c.Modifiers.Any(SyntaxKind.PartialKeyword)))
            Error(type, "An entity must be a top-level, non-generic, non-abstract partial class.");
        if (!type.InstanceConstructors.Any(c => c.Parameters.Length == 0 && c.DeclaredAccessibility == Accessibility.Public))
            Error(type, "An entity needs a public parameterless constructor for AOT-safe typed reads.");
        if (model.Prefix.Length == 0 || model.Prefix.IndexOf('\0') >= 0)
            Error(type, "FqEntity needs a nonempty stable key prefix without NUL.");
        for (var parent = type.BaseType; parent != null; parent = parent.BaseType)
            if (parent.GetMembers().Any(m => Attribute(m, FieldAttribute) != null))
                Error(type, "Inherited FqField members are unsupported. Declare the persistent fields in this entity.");
        var numbers = new HashSet<int>();
        foreach (var member in type.GetMembers()) {
            var field = Attribute(member, FieldAttribute);
            bool key = Attribute(member, KeyAttribute) != null;
            if (field == null) {
                if (key) Error(member, "FqKey must also have FqField so the ID is stored in the record.");
                continue;
            }
            ITypeSymbol? memberType = member is IFieldSymbol f ? f.Type : (member as IPropertySymbol)?.Type;
            if (member.IsStatic || memberType == null || member is IFieldSymbol { IsReadOnly: true } ||
                member is IFieldSymbol { IsConst: true } ||
                member is IPropertySymbol p && (p.IsIndexer || p.GetMethod == null || p.SetMethod == null || p.SetMethod.IsInitOnly)) {
                Error(member, "FqField requires a writable instance field or a readable and writable property without init.");
                continue;
            }
            int number = field.ConstructorArguments.Length == 1 && field.ConstructorArguments[0].Value is int n ? n : 0;
            if (number <= 0 || number > 536870911 || number >= 19000 && number <= 19999 || !numbers.Add(number)) {
                Error(member, "FqField numbers must be unique protobuf numbers in 1..536870911, excluding 19000..19999.");
                continue;
            }
            var underlying = memberType is INamedTypeSymbol named && named.OriginalDefinition.SpecialType == SpecialType.System_Nullable_T
                ? named.TypeArguments[0] : memberType;
            string? kind = Kind(underlying);
            if (kind == null) {
                Error(member, "Unsupported FqField type " + memberType.ToDisplayString() + ". Supported fields are integers, bool, float, double, string, byte[] and nullable scalar values.");
                continue;
            }
            var value = new Member {
                Name = member.Name, Number = number, Kind = kind, Key = key,
                Type = memberType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat.WithMiscellaneousOptions(
                    SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers | SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier)),
                ValueType = underlying.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                Optional = !SymbolEqualityComparer.Default.Equals(memberType, underlying),
                NullableReference = memberType.NullableAnnotation == NullableAnnotation.Annotated
            };
            if (key && (value.Optional || value.NullableReference || kind != "signed" && kind != "unsigned" && kind != "string"))
                Error(member, "FqKey must be a non-nullable integer or string.");
            model.Members.Add(value);
        }
        if (model.Members.Count(m => m.Key) != 1) Error(type, "An entity requires exactly one FqKey member.");
        var reserved = new HashSet<string>(new[] { "EncodedSize", "EncodeTo", "Decode", "MakeKey", "Cols", "View" });
        foreach (var member in model.Members) reserved.Add("Read" + member.Name);
        foreach (var member in type.GetMembers())
            if (!model.Manual && (reserved.Contains(member.Name) || member.Name.StartsWith("__fq", StringComparison.Ordinal)))
                Error(member, "Member name " + member.Name + " is reserved by the FengquDB generator.");
        model.Members.Sort((a, b) => a.Number.CompareTo(b.Number));
        return model;
    }
    static string Value(Member member, string owner) => owner + ".@" + member.Name + (member.Optional ? ".Value" : "");
    static string Default(Member member) => member.Optional || member.NullableReference ? "default"
        : member.Kind == "string" ? "global::System.String.Empty"
        : member.Kind == "bytes" ? "global::System.Array.Empty<byte>()" : "default";
    static string Size(Member member, string value) {
        switch (member.Kind) {
        case "unsigned": return Wire + ".VarintSize((ulong)" + value + ")";
        case "signed": return Wire + ".VarintSize(" + Wire + ".ZigZag((long)" + value + "))";
        case "bool": return "1";
        case "double": return "8";
        case "single": return "4";
        case "string": return Wire + ".StringSize(" + value + ")";
        default: return Wire + ".BytesSize(" + value + ".Length)";
        }
    }
    static string Write(Member member, string output, string value) {
        switch (member.Kind) {
        case "unsigned": return Wire + ".WriteVarint(" + output + ", (ulong)" + value + ")";
        case "signed": return Wire + ".WriteVarint(" + output + ", " + Wire + ".ZigZag((long)" + value + "))";
        case "bool": return Wire + ".WriteVarint(" + output + ", " + value + " ? 1UL : 0UL)";
        case "double": return Wire + ".WriteDouble(" + output + ", " + value + ")";
        case "single": return Wire + ".WriteSingle(" + output + ", " + value + ")";
        case "string": return Wire + ".WriteString(" + output + ", " + value + ")";
        default: return Wire + ".WriteBytes(" + output + ", " + value + ")";
        }
    }
    static string Read(Member member, string input) {
        switch (member.Kind) {
        case "unsigned": return "checked((" + member.ValueType + ")" + Wire + ".ReadVarint(" + input + ", ref position))";
        case "signed": return "checked((" + member.ValueType + ")" + Wire + ".UnZigZag(" + Wire + ".ReadVarint(" + input + ", ref position)))";
        case "bool": return Wire + ".ReadBool(" + input + ", ref position)";
        case "double": return Wire + ".ReadDouble(" + input + ", ref position)";
        case "single": return Wire + ".ReadSingle(" + input + ", ref position)";
        case "string": return Wire + ".Utf8.GetString(" + Wire + ".ReadBytes(" + input + ", ref position))";
        default: return Wire + ".ReadBytes(" + input + ", ref position).ToArray()";
        }
    }
    static string Emit(Model model) {
        var code = new StringBuilder("// <auto-generated/>\n#nullable enable\nusing System;\n");
        if (model.Namespace.Length != 0) code.Append("namespace ").Append(model.Namespace).AppendLine(" {");
        code.Append("[global::FengquDB.Entities.FqGeneratedEntityAttribute(\"").Append(Fingerprint(model)).AppendLine("\")]");
        code.Append("partial class @").Append(model.Name).Append(" : global::FengquDB.Entities.IFqEntity<").Append(model.FullName).AppendLine("> {");
        code.AppendLine("    private byte[]? __fqUnknown;");
        code.Append("    private static readonly byte[] __fqPrefix = ").Append(Wire).Append(".Utf8.GetBytes(")
            .Append(SymbolDisplay.FormatLiteral(model.Prefix, true)).AppendLine(");");
        EmitSize(code, model, false);
        EmitSize(code, model, true);
        code.AppendLine("    public int EncodeTo(global::System.Span<byte> destination) {\n        int position = 0;");
        foreach (var member in model.Members) {
            string value = Value(member, "this");
            string condition = member.Optional ? "this.@" + member.Name + ".HasValue" : "this.@" + member.Name + " != null";
            if (member.Optional || member.Reference) code.Append("        if (").Append(condition).AppendLine(") {");
            code.Append("        position += ").Append(Wire).Append(".WriteVarint(destination.Slice(position), ").Append(member.Tag).AppendLine("UL);");
            code.Append("        position += ").Append(Write(member, "destination.Slice(position)", value)).AppendLine(";");
            if (member.Optional || member.Reference) code.AppendLine("        }");
        }
        code.AppendLine("        if (__fqUnknown != null) { __fqUnknown.AsSpan().CopyTo(destination.Slice(position)); position += __fqUnknown.Length; }\n        return position;\n    }");
        code.Append("    public static ").Append(model.FullName).AppendLine(" Decode(global::System.ReadOnlySpan<byte> source) {");
        foreach (var member in model.Members)
            code.Append("        ").Append(member.Type).Append(" __fqValue").Append(member.Number)
                .Append(" = ").Append(Default(member)).AppendLine(";");
        code.AppendLine("        int position = 0, unknownSize = 0;\n        while (position < source.Length) {\n            int begin = position;");
        code.Append("            uint tag = ").Append(Wire).AppendLine(".ReadTag(source, ref position);\n            switch (tag) {");
        foreach (var member in model.Members)
            code.Append("            case ").Append(member.Tag).Append("u: __fqValue").Append(member.Number)
                .Append(" = ").Append(Read(member, "source")).AppendLine("; break;");
        code.Append("            default: ").Append(Wire).AppendLine(".Skip(source, ref position, tag); unknownSize = checked(unknownSize + position - begin); break;\n            }\n        }");
        code.Append("        var result = new ").Append(model.FullName).AppendLine("();");
        foreach (var member in model.Members)
            code.Append("        result.@").Append(member.Name).Append(" = __fqValue").Append(member.Number).AppendLine(";");
        code.Append("        if (unknownSize != 0) result.__fqUnknown = ").Append(Wire).AppendLine(".CopyUnknown(source, unknownSize, __fqKnown);\n        return result;\n    }");
        code.Append("    private static readonly global::System.Func<uint, bool> __fqKnown = tag => ")
            .Append(string.Join(" || ", model.Members.Select(m => "tag == " + m.Tag + "u"))).AppendLine(";");
        string contract = "global::FengquDB.Entities.IFqEntity<" + model.FullName + ">";
        code.Append("    ").Append(model.FullName).Append(' ').Append(contract).AppendLine(".ReadEntity(global::System.ReadOnlySpan<byte> source) => Decode(source);");
        code.Append("    global::System.ReadOnlySpan<byte> ").Append(contract).AppendLine(".KeyPrefix => __fqPrefix;");
        var key = model.Members.Single(m => m.Key);
        string keyKind = key.Kind == "string" ? "Text" : key.Kind == "signed" ? "Signed" : "Unsigned";
        code.Append("    global::FengquDB.Entities.FqEntityKeyKind ").Append(contract)
            .Append(".KeyKind => global::FengquDB.Entities.FqEntityKeyKind.").Append(keyKind).AppendLine(";");
        EmitWrite(code, contract, key, false);
        EmitWrite(code, contract, key, true);
        code.Append("    public static byte[] MakeKey(").Append(key.Type).AppendLine(" id) {");
        string keyArgument = key.Kind == "string" ? "id" : "(" + (key.Kind == "signed" ? "long" : "ulong") + ")id";
        code.Append("        int size = ").Append(Wire).Append(".KeySize(__fqPrefix, ").Append(keyArgument).AppendLine(");\n        byte[]? rented = null;\n        global::System.Span<byte> buffer = size <= 512 ? stackalloc byte[512] : (rented = global::System.Buffers.ArrayPool<byte>.Shared.Rent(size));\n        try {");
        code.Append("            int written = ").Append(Wire).Append(".WriteKey(buffer, __fqPrefix, ").Append(keyArgument).AppendLine(");\n            return buffer.Slice(0, written).ToArray();\n        } finally { if (rented != null) global::System.Buffers.ArrayPool<byte>.Shared.Return(rented); }\n    }");
        foreach (var member in model.Members) {
            code.Append("    public static ").Append(member.Type).Append(" Read").Append(member.Name).AppendLine("(global::System.ReadOnlySpan<byte> source) {");
            code.Append("        ").Append(member.Type).Append(" value = ").Append(Default(member)).AppendLine(";");
            code.AppendLine("        int position = 0;\n        while (position < source.Length) {");
            code.Append("            uint tag = ").Append(Wire).AppendLine(".ReadTag(source, ref position);");
            code.Append("            if (tag == ").Append(member.Tag).Append("u) value = ").Append(Read(member, "source")).AppendLine(";");
            code.Append("            else ").Append(Wire).AppendLine(".Skip(source, ref position, tag);\n        }\n        return value;\n    }");
        }
        code.AppendLine("    public readonly ref struct View {\n        private readonly global::System.ReadOnlySpan<byte> __fqSource;\n        public View(global::System.ReadOnlySpan<byte> source) { __fqSource = source; }");
        foreach (var member in model.Members)
            code.Append("        public ").Append(member.Type).Append(" @").Append(member.Name).Append(" => Read").Append(member.Name).AppendLine("(__fqSource);");
        code.AppendLine("    }\n    public static class Cols {");
        foreach (var member in model.Members) {
            if (member.Kind == "bytes") continue;
            string factory = member.Kind == "unsigned" ? "U64" : member.Kind == "signed" ? "S64"
                : member.Kind == "string" ? "Text" : member.Kind == "bool" ? "Bool" : member.Kind == "single" ? "F32" : "F64";
            string columnType = member.Kind == "unsigned" ? "ulong" : member.Kind == "signed" ? "long"
                : member.Kind == "string" ? "string" : member.Kind == "bool" ? "bool" : "double";
            code.Append("        public static readonly global::FengquDB.QCol<").Append(columnType).Append("> @").Append(member.Name)
                .Append(" = global::FengquDB.Q.").Append(factory).Append('(').Append(member.Number).AppendLine("u);");
        }
        code.AppendLine("    }\n}");
        if (model.Namespace.Length != 0) code.AppendLine("}");
        return code.ToString();
    }
    static string Fingerprint(Model model) {
        var schema = new StringBuilder("fqdb-csharp-1\n");
        void Text(string value) => schema.Append(value.Length.ToString(System.Globalization.CultureInfo.InvariantCulture))
            .Append(':').Append(value).Append('\n');
        Text(model.FullName);
        Text(model.Prefix);
        foreach (var member in model.Members) {
            Text(member.Name); Text(member.Type); Text(member.Kind);
            Text(member.Number.ToString(System.Globalization.CultureInfo.InvariantCulture));
            Text(member.Key ? "key" : "field");
            Text(member.Optional ? "optional" : "value");
            Text(member.NullableReference ? "nullable" : "nonnull");
        }
        using (var hash = System.Security.Cryptography.SHA256.Create())
            return BitConverter.ToString(hash.ComputeHash(Encoding.UTF8.GetBytes(schema.ToString()))).Replace("-", "");
    }
    static void EmitSize(StringBuilder code, Model model, bool capacity) {
        code.Append(capacity ? "    private int __fqCapacity() {\n" : "    public int EncodedSize() {\n")
            .AppendLine("        int size = __fqUnknown?.Length ?? 0;\n        checked {");
        foreach (var member in model.Members) {
            string value = Value(member, "this");
            string condition = member.Optional ? "this.@" + member.Name + ".HasValue" : "this.@" + member.Name + " != null";
            bool conditional = member.Optional || member.Reference;
            if (conditional) code.Append("            if (").Append(condition).AppendLine(") {");
            // One UTF-16 code unit needs at most three UTF-8 bytes. A small
            // safe upper bound avoids an otherwise redundant UTF-8 length scan.
            string bytes = capacity && member.Kind == "string"
                ? Wire + ".BytesSize(checked(" + value + ".Length * 3))" : Size(member, value);
            code.Append(conditional ? "                " : "            ")
                .Append("size += ").Append(TagSize(member.Tag)).Append(" + ").Append(bytes).AppendLine(";");
            if (conditional) code.AppendLine("            }");
        }
        code.AppendLine("        }\n        return size;\n    }");
    }
    static void EmitWrite(StringBuilder code, string contract, Member key, bool transaction) {
        string targetType = transaction ? "global::FengquDB.StoreTransaction" : "global::FengquDB.Store";
        string resultType = transaction ? "void" : "ulong";
        string reliability = transaction ? "" : ", bool forceReliable";
        string reliabilityArgument = transaction ? "" : ", forceReliable";
        string idType = key.Kind == "string" ? "string" : key.Kind == "signed" ? "long" : "ulong";
        string id = key.Kind == "string" ? "this.@" + key.Name : "(" + idType + ")this.@" + key.Name;
        code.Append("    ").Append(resultType).Append(' ').Append(contract).Append(".WriteTo(")
            .Append(targetType).Append(" store, ulong expiresAtMs").Append(reliability).AppendLine(") {");
        code.Append("        ").Append(idType).Append(" __fqId = ").Append(id).AppendLine(";");
        code.Append("        int keySize = ").Append(Wire).AppendLine(".KeySize(__fqPrefix, __fqId);");
        code.AppendLine("        int size = __fqCapacity();\n        if (checked(keySize + size) > 512) {\n            size = EncodedSize();\n            if (checked(keySize + size) > 512) {");
        code.Append("            ").Append(transaction ? "" : "return ")
            .Append("__fqWriteLarge(store, __fqId, keySize, size, expiresAtMs").Append(reliabilityArgument).AppendLine(");");
        if (transaction) code.AppendLine("            return;");
        code.AppendLine("            }\n        }\n        global::System.Span<byte> buffer = stackalloc byte[512];");
        EmitWriteBody(code, transaction, "        ");
        code.AppendLine("    }");
        code.Append("    private ").Append(resultType).Append(" __fqWriteLarge(")
            .Append(targetType).Append(" store, ").Append(idType)
            .Append(" __fqId, int keySize, int size, ulong expiresAtMs").Append(reliability).AppendLine(") {");
        code.AppendLine("        global::FengquDB.Entities.FqEntityStorage.ValidateSize(keySize, size);");
        code.AppendLine("        byte[] rented = global::System.Buffers.ArrayPool<byte>.Shared.Rent(checked(keySize + size));\n        try {\n            global::System.Span<byte> buffer = rented;");
        EmitWriteBody(code, transaction, "            ");
        code.AppendLine("        } finally { global::System.Buffers.ArrayPool<byte>.Shared.Return(rented); }\n    }");
    }
    static void EmitWriteBody(StringBuilder code, bool transaction, string indent) {
        code.Append(indent).Append("int keyBytes = ").Append(Wire).AppendLine(".WriteKey(buffer.Slice(0, keySize), __fqPrefix, __fqId);");
        code.Append(indent).AppendLine("int written = EncodeTo(buffer.Slice(keySize, size));");
        code.Append(indent).Append(transaction ? "" : "return ")
            .Append("store.Put(buffer.Slice(0, keyBytes), buffer.Slice(keySize, written), expiresAtMs")
            .Append(transaction ? ");" : ", forceReliable).Version;").AppendLine();
    }
    static int TagSize(uint tag) {
        int size = 1;
        while (tag >= 128) { tag >>= 7; ++size; }
        return size;
    }
}
}
