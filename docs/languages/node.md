# Node.js / TypeScript 接入

[API、用途和调用例子](../api/node.md)

应用启动时打开数据库句柄，交给各业务模块复用，退出时统一关闭。
## 1. 放好 SDK 并跑通存取

下载本语言独立 ZIP，把 SDK 解压为项目根目录的 `FengquDB`。包内已有 JavaScript 包装及 `.node` 原生模块，先用普通 JavaScript 验证：

```text
项目/
  app.cjs
  student.js
  package.json
  FengquDB/
  FengquDB/Tools/fqdb-codegen.exe           ← Windows 工具
```

`app.cjs`：

```js
const { Store } = require('./FengquDB');
require('node:fs').mkdirSync('save', {recursive:true});
const db = Store.open('save/main');
try {
  db.put('name', '张三');
  console.log(Buffer.from(db.get('name')).toString('utf8'));
  db.sync();
} finally { db.close(); }
```

在项目目录运行 `node app.cjs`，看到 `张三` 即成功。SDK 的原生模块和引擎已经放在一起，原生模块会从包内加载引擎。

## 2. 从自己的类生成

`student.js`：

```js
// fqdb:entity prefix=student/ key=id
class Student {
  id = 0;
  name = '';
  // fqdb:field type=sint32
  score = 0;
}
module.exports = {Student};
```

按下节双击脚本，生成 `Generated/node/models/fq_codecs.js`。在 app.cjs 顶部加入：

```js
const { Student } = require('./student');
const { StudentFqCodec: Codec } = require('./Generated/node/models/fq_codecs');
```

在 Store 打开后的 try 内加入：

```js
const student = new Student();
student.id=7; student.name='张三'; student.score=95;
db.put(Codec.key(student), Codec.encode(student));
const saved = Codec.decode(db.get(Codec.key(student))).value;
console.log(saved.name, saved.score);
```

再运行 `node app.cjs`，会看到 `张三 95`。

## 3. 跟随运行或 TypeScript 构建更新

JavaScript 项目在已有 package.json 的 scripts 中加入以下内容，之后用 `npm start` 运行：

```json
"prestart": "node FengquDB/fqdb-generate.cjs",
"start": "node app.cjs"
```

TypeScript 项目改放在 `prebuild`：`"prebuild": "node FengquDB/fqdb-generate.cjs"`，然后使用原来的 `npm run build`。使用 npm run build 触发前置生成。

TypeScript 实体放在如 `src/student.ts` 中，使用 `export class Student`、明确的字段类型和相同标记。生成目录统一为 `Generated/node`。tsconfig 启用 `allowJs`，让 `src` 和 `Generated/node/**/*.js` 参与编译，并保持两者在输出目录中的相对位置（rootDir 使用项目根目录）。从 `src` 导入 codec 时使用 `../Generated/node/models/fq_codecs`。超过 number 安全范围的整数使用 bigint；需要固定整数编码的 number 字段用 `fqdb:field type=sint32` 标明。

## 手动生成

生成工具位于 `FengquDB` 文件夹内。Windows 双击 `FengquDB/fqdb-generate.bat`；macOS 双击 `FengquDB/fqdb-generate.command`；Linux 在项目目录执行 `sh FengquDB/fqdb-generate.command`。

脚本自动定位业务项目，使用包内 Tools 目录的生成器。成功显示 `Entity codecs are up to date.`，首次输出在项目 `Generated`；已有自动生成配置时复用它的输出位置。实体修改后再次编译或双击即可。`FengquDB.schema.lock.json` 保存字段编号，随业务代码保存。

## 可选：存档路径、多个数据库和写入模式

前面的接入示例已包含默认配置。需要调整时参考本节。

根目录由项目设置，使用可写的存档目录。不同子目录就是不同数据库：

```js
const path = require('node:path');
const root = path.join(__dirname, 'save'); // 换成宿主提供的存档目录
require('node:fs').mkdirSync(root, {recursive:true});
const players = Store.open(path.join(root, 'players'));
try {
  const world = Store.open(path.join(root, 'world'));
  try {
    players.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024});
    players.put('name', '张三');
    players.configureWrite({mode:'reliable'}); // 这个库改为每笔可靠落盘
    // players.resetWriteOptions(); // 恢复默认性能模式、50 ms / 1 MiB
  } finally { world.close(); }
} finally { players.close(); }
```

新库默认性能模式：内存写入成功即可返回并读取新值，按周期或字节阈值刷盘，空闲不空刷；断电或崩溃可能丢失尚未落盘的数据。可靠模式等待落盘成功再返回。数据和配置按库独立保存，配置重开后保留；切换可能等待刷盘，建议初始化时设置。同一库打开后复用。可靠写入和配置切换可能阻塞事件循环，服务器可放到 worker 执行。

发布带业务代码、生成代码、Node 客户端及该运行平台的 `.node` 和引擎文件。生成器及 prebuild/prestart 步骤只在开发/构建时使用；打好的应用启动不应再次依赖生成工具。
