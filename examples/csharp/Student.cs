using FengquDB.Entities;

namespace FengquDB.Examples {
// Stable field numbers are the schema. The generator emits protobuf code,
// typed query columns, key formatting and allocation-free scalar accessors.
[FqEntity("student/")]
public partial class Student {
    [FqKey, FqField(1)] public int Id { get; set; }
    [FqField(2)] public string Name { get; set; } = "";
    [FqField(3)] public int Score { get; set; }
}
}
