'use strict';
const fs = require('node:fs');
const path = require('node:path');

async function main() {
  const [modulePath, directory, ...args] = process.argv.slice(2);
  if (!modulePath || !directory || args.indexOf('-o') < 0) {
    throw new Error('Usage: node run-linker.cjs module.js directory ld-arguments-with-output');
  }
  const output = args[args.indexOf('-o') + 1];
  if (!output || output !== path.basename(output)) throw new Error('Output must be a filename');
  const factory = require(path.resolve(modulePath));
  const instance = await factory({ noInitialRun: true, print: console.log, printErr: console.error });
  instance.FS.mkdir('/work');
  instance.FS.chdir('/work');
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    if (entry.isFile() && entry.name !== output) {
      instance.FS.writeFile(entry.name, fs.readFileSync(path.join(directory, entry.name)));
    }
  }
  let code = 0;
  try {
    code = instance.callMain(args) || 0;
  } catch (error) {
    if (error.name !== 'ExitStatus') throw error;
    code = error.status;
  }
  if (code) throw new Error(`Linker exited ${code}`);
  fs.writeFileSync(path.join(directory, output), instance.FS.readFile(output));
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
