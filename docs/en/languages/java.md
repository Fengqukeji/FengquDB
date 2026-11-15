# Java getting started

[简体中文](../../languages/java.md)

[API and examples](../api/java.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Place the SDK and reference the JAR

Download this language's SDK ZIP and extract its `FengquDB` folder into your project root. The example uses an existing Gradle Java project with this layout:

```text
project/
  build.gradle
  src/main/java/example/Student.java
  src/main/java/example/Main.java
  FengquDB/fengqudb.jar
  FengquDB/fengqudb.gradle
  FengquDB/Tools/fqdb-codegen.exe             <- Windows tool
```

Enable the Java/application plugins in `build.gradle` and apply the SDK configuration:

```groovy
plugins { id 'application' }
apply from: 'FengquDB/fengqudb.gradle'
application { mainClass = 'example.Main' }
```

Merge this with your existing plugin configuration. The SDK script references the JAR, sets the runtime native library path, and generates entity code before compileJava.

## 2. Define a student class

`src/main/java/example/Student.java`:

```java
package example;
// fqdb:entity prefix=student/ key=id
public class Student {
    public int id;
    public String name = "";
    public int score;
}
```

## 3. Call the database

`src/main/java/example/Main.java`:

```java
package example;
import io.fengqudb.Store;
public class Main {
    public static void main(String[] args) {
        try (Store db = Store.openAt("save", "main")) {
            Student student = new Student();
            student.id=7; student.name="Alice"; student.score=95;
            byte[] key = StudentFqCodec.key(student);
            db.put(key, StudentFqCodec.encode(student));
            Student saved = StudentFqCodec.decode(db.get(key)).value;
            System.out.println(saved.name + " " + saved.score);
            db.sync();
        }
    }
}
```

Run your existing `gradlew.bat run` on Windows or `./gradlew run` on macOS/Linux from the project root, or use Gradle run in your IDE. The output `Alice 95` confirms success. Generated code is under `Generated/java/example`, and data is under `save/main` in the working directory. Recompile after changing the class to update its codec.

For IDE integration, add FengquDB/fengqudb.jar as a dependency, run manual generation below, and add Generated/java as a source directory. The JAR loads the platform engine from the adjacent Native directory.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Set a writable data directory in your application. Different subdirectories are independent databases:

```java
import java.nio.file.Path;
import java.nio.file.Paths;
import io.fengqudb.Store;

Path root = Paths.get("save"); // Replace with the host's writable data directory
try (Store players = Store.openAt(root.toString(), "players");
     Store world = Store.openAt(root.toString(), "world")) {
    Store.WriteOptions options = players.writeOptions();
    options.mode = Store.WriteMode.PERFORMANCE;
    options.periodMs = 50;
    options.thresholdBytes = 1024L * 1024;
    players.configureWrite(options);

    options.mode = Store.WriteMode.RELIABLE;
    players.configureWrite(options); // Require durable storage for every write to this database
    // players.resetWriteOptions(); // Restore default performance mode, 50 ms / 1 MiB
}
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Each database stores its data and settings separately; settings survive reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle.

Deploy your application and generated classes, preserving the relative layout of FengquDB/fengqudb.jar and FengquDB/Native. If you merge them into one application JAR, set `-Dfengqudb.nativeDir=/path/to/native` to specify the engine directory.
