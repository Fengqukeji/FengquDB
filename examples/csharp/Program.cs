using System;
using System.IO;
using FengquDB;
using FengquDB.Examples;

using var db = new Store(args.Length == 0 ? Path.Combine(AppContext.BaseDirectory, "students-db") : args[0]);
db.Put(new Student { Id = 1, Name = "张三", Score = 95 });
var student = db.GetEntity<Student>(1);
Console.WriteLine($"{student!.Id} {student.Name} {student.Score}");

db.Put("score", 95);
db.Put("name", "张三");
Console.WriteLine($"{db.Get<int>("score")} {db.Get<string>("name")}");
if (!db.TryGet<int>("missing", out _)) Console.WriteLine("missing has no value");

// The same generated field numbers and physical types drive native queries.
var score = Student.Cols.Score;
var name = Student.Cols.Name;
foreach (var row in Q.From("student/").Where(score >= 90L).Select(name, score).OrderBy(score.Desc()).Run(db))
    Console.WriteLine($"High score: {row.Get(name)} {row.Get(score)}");

// Repeated reads of one key avoid encoding the key on every call.
using (var reader = db.PrepareValue<int>("score")) Console.WriteLine(reader.Read());
using (var reader = db.PrepareReader(Student.MakeKey(1)))
    Console.WriteLine(reader.Read(rows => Student.ReadScore(rows[0].Value)));

// Critical writes can request durability independently of the default mode.
db.Put("paid-order", 123L, forceReliable: true);
db.Sync();
