// Invoked as a prebuild script from the project root; leave this file in the SDK.
const { existsSync } = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = process.cwd();
const local = path.join(__dirname, 'Tools', process.platform === 'win32' ? 'fqdb-codegen.exe' : 'fqdb-codegen');
const compiler = process.env.FQDB_CODEGEN || (existsSync(local) ? local : 'fqdb-codegen');
const result = spawnSync(compiler, ['generate', '--source', root, '--source-language', 'node',
    '--output', path.join(root, 'Generated'), '--schema-lock', path.join(root, 'FengquDB.schema.lock.json')],
    { stdio: 'inherit', shell: false });
if (result.error) console.error(result.error.message);
process.exit(result.status === null ? 1 : result.status);
