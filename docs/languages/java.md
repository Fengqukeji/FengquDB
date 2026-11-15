# Java 接入

[API、用途和调用例子](../api/java.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 放好 SDK 并引用 JAR

下载本语言独立 ZIP，把 SDK 解压为项目根目录的 `FengquDB`。下面按已有 Gradle Java 项目说明；文件位置是：

```text
项目/
  build.gradle
  src/main/java/example/Student.java
  src/main/java/example/Main.java
  FengquDB/fengqudb.jar
  FengquDB/fengqudb.gradle
  FengquDB/Tools/fqdb-codegen.exe             ← Windows 工具
```

在 `build.gradle` 启用 Java/application 插件，并应用随包配置：

```groovy
plugins { id 'application' }
apply from: 'FengquDB/fengqudb.gradle'
application { mainClass = 'example.Main' }
```

将上述内容合并到已有插件配置中。随包脚本会引用 JAR、设置运行时原生库路径，并在 compileJava 前生成实体代码。

## 2. 定义学生类

`src/main/java/example/Student.java`：

```java
package example;
// fqdb:entity prefix=student/ key=id
public class Student {
    public int id;
    public String name = "";
    public int score;
}
```

## 3. 调用数据库

`src/main/java/example/Main.java`：

```java
package example;
import io.fengqudb.Store;
public class Main {
    public static void main(String[] args) {
        try (Store db = Store.openAt("save", "main")) {
            Student student = new Student();
            student.id=7; student.name="张三"; student.score=95;
            byte[] key = StudentFqCodec.key(student);
            db.put(key, StudentFqCodec.encode(student));
            Student saved = StudentFqCodec.decode(db.get(key)).value;
            System.out.println(saved.name + " " + saved.score);
            db.sync();
        }
    }
}
```

在项目根目录运行已有的 `gradlew.bat run`（Windows）或 `./gradlew run`（macOS/Linux），也可以用 IDE 的 Gradle run。看到 `张三 95` 即成功；生成代码在 `Generated/java/example`，存档在运行目录下的 `save/main`。修改类后再次编译就会更新编码。

IDE 接入时，将 FengquDB/fengqudb.jar 加到工程依赖，按下节双击生成后把 Generated/java 加入源码目录。JAR 会自动从旁边的 Native 目录加载本平台引擎。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

路径由项目设置，使用可写的存档目录。不同子目录就是不同数据库：

```java
import java.nio.file.Path;
import java.nio.file.Paths;
import io.fengqudb.Store;

Path root = Paths.get("save"); // 换成宿主提供的存档目录
try (Store players = Store.openAt(root.toString(), "players");
     Store world = Store.openAt(root.toString(), "world")) {
    Store.WriteOptions options = players.writeOptions();
    options.mode = Store.WriteMode.PERFORMANCE;
    options.periodMs = 50;
    options.thresholdBytes = 1024L * 1024;
    players.configureWrite(options);

    options.mode = Store.WriteMode.RELIABLE;
    players.configureWrite(options); // 这个库改为每笔可靠落盘
    // players.resetWriteOptions(); // 恢复默认性能模式、50 ms / 1 MiB
}
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。数据和配置按库独立保存，配置重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。

发布保留 FengquDB/fengqudb.jar 和 FengquDB/Native 的相对位置，并带上业务程序与生成类。合并为单个业务 JAR 时，可用启动参数 `-Dfengqudb.nativeDir=原生库目录` 指定引擎位置。
