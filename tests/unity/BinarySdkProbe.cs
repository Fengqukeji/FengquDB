using FengquDB.Entities;

[FqEntity("student/")]
public partial class BinaryStudent {
    [FqKey, FqField(1)] public int Id { get; set; }
    [FqField(2)] public string Name { get; set; } = "";
    [FqField(3)] public int Score { get; set; }
}

public static class BinarySdkProbe {
    public static int Roundtrip() {
        var student = new BinaryStudent { Id = 7, Name = "Alice", Score = 95 };
        var bytes = new byte[student.EncodedSize()];
        student.EncodeTo(bytes);
        return BinaryStudent.Decode(bytes).Score;
    }
}
