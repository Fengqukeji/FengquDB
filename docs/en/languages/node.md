# Node.js / TypeScript getting started

[简体中文](../../languages/node.md)

[API and examples](../api/node.md)

Open a database handle at application startup, share it across application modules, and close it at shutdown.
## 1. Place the SDK and run a read and write

Download this language's SDK ZIP and extract its `FengquDB` folder into your project root. It includes the JavaScript wrapper and `.node` native module. Start with plain JavaScript:

```text
project/
  app.cjs
  student.js
  package.json
  FengquDB/
  FengquDB/Tools/fqdb-codegen.exe           <- Windows tool
```

`app.cjs`:

```js
const { Store } = require('./FengquDB');
require('node:fs').mkdirSync('save', {recursive:true});
const db = Store.open('save/main');
try {
  db.put('name', 'Alice');
  console.log(Buffer.from(db.get('name')).toString('utf8'));
  db.sync();
} finally { db.close(); }
```

Run `node app.cjs` from the project directory; `Alice` confirms success. The native module and engine are already colocated in the SDK, and the module loads the engine from there.

## 2. Generate from your own class

`student.js`:

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

Double-click the script described below to generate `Generated/node/models/fq_codecs.js`. Add this at the top of app.cjs:

```js
const { Student } = require('./student');
const { StudentFqCodec: Codec } = require('./Generated/node/models/fq_codecs');
```

Inside try, after opening the Store, add:

```js
const student = new Student();
student.id=7; student.name='Alice'; student.score=95;
db.put(Codec.key(student), Codec.encode(student));
const saved = Codec.decode(db.get(Codec.key(student))).value;
console.log(saved.name, saved.score);
```

Run `node app.cjs` again; the output includes `Alice 95`.

## 3. Regenerate on startup or TypeScript builds

In a JavaScript project, add this to scripts in your existing package.json, then run with `npm start`:

```json
"prestart": "node FengquDB/fqdb-generate.cjs",
"start": "node app.cjs"
```

In a TypeScript project, use `prebuild`: `"prebuild": "node FengquDB/fqdb-generate.cjs"`, then run your usual `npm run build` to trigger generation before building.

Place TypeScript entities in a file such as `src/student.ts`, using `export class Student`, explicit field types, and the same marker. Output is under `Generated/node`. Enable `allowJs` in tsconfig; compile both `src` and `Generated/node/**/*.js` while preserving their relative output paths (set rootDir to the project root). From `src`, import the codec as `../Generated/node/models/fq_codecs`. Use bigint for integers outside number's safe range. Mark number fields that need fixed integer encoding with `fqdb:field type=sint32`.

## Manual generation

The tool is inside the `FengquDB` folder. On Windows, double-click `FengquDB/fqdb-generate.bat`; on macOS, double-click `FengquDB/fqdb-generate.command`; on Linux, run `sh FengquDB/fqdb-generate.command` from the project directory.

The script locates the application project and uses the generator in the SDK's Tools directory. Success prints `Entity codecs are up to date.` The initial output is under `Generated`; existing automatic-generation settings are reused. After editing entities, build again or double-click the script. Keep `FengquDB.schema.lock.json`, which preserves field numbers, with your application sources.

## Optional: data path, multiple databases, and write mode

The examples above use the default settings. Refer to this section when you want to change them.

Set a writable data directory in your application. Different subdirectories are independent databases:

```js
const path = require('node:path');
const root = path.join(__dirname, 'save'); // Replace with the host's writable data directory
require('node:fs').mkdirSync(root, {recursive:true});
const players = Store.open(path.join(root, 'players'));
try {
  const world = Store.open(path.join(root, 'world'));
  try {
    players.configureWrite({mode:'performance', periodMs:50, thresholdBytes:1024*1024});
    players.put('name', 'Alice');
    players.configureWrite({mode:'reliable'}); // Require durable storage for every write to this database
    // players.resetWriteOptions(); // Restore default performance mode, 50 ms / 1 MiB
  } finally { world.close(); }
} finally { players.close(); }
```

New databases default to performance mode: writes return once applied in memory, and new values are immediately readable. Flushing occurs at the period or byte threshold, with no empty flushes while idle. A crash or power loss can lose unflushed data. Reliable mode waits for durable storage before returning. Each database stores data and settings separately; settings survive reopening. Switching modes can wait for a flush, so configure at initialization when possible. Reuse the open database handle. Reliable writes and configuration changes can block the event loop; servers can run them in a worker.

Deploy application code, generated code, the Node client, and the `.node` module and engine for the target platform. Use the generator and prebuild/prestart steps during development/builds; the published application should not depend on running the generator at startup.
