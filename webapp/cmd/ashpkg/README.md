# ashpkg

Build with `go build -o build/ashpkg ./cmd/ashpkg` from `webapp`.

```sh
build/ashpkg resolve -request resolve.json -output lock.json
build/ashpkg build -request build.json
```

A resolve request contains `catalog` (complete recipes) and `requests`
(environment selections). The runnable `testdata/resolve.json` fixture shows the
format. Omitting `-output` writes the lock to stdout.

A build request selects one binding from the complete validated lock:

```json
{
  "lock": "lock.json",
  "environmentID": "first",
  "packageID": "amiga.fixture",
  "tier": "environment",
  "inputs": {},
  "info": {
    "package": "ASHtest",
    "name": "Test application",
    "version": "1",
    "architecture": "m68k",
    "baseDir": "/amiga/apps/test",
    "timestamp": 0
  },
  "output": "fixture.pkg",
  "receipt": "receipt.json"
}
```

`inputs` maps source SHA-256 digests to local media paths. Paths inside requests
are relative to the request file; `-output` is relative to the current directory.
Both outputs must be new files. Source hashes, dependency closure, recipe status,
and metadata are checked before package publication. The CLI creates a real SVR4
`.pkg` and its JSON receipt; it does not install anything on the host.

The browser worker API is synchronous `auxPackages(json, readers, write)`:

- Resolve: one argument, `{action:"resolve", catalog:[...], requests:[...]}`;
  returns JSON `{ok:true, lock}`.
- Build: `{action:"build", lock, environmentID, packageID, tier, info,
  inputs:[{sha256,size}]}` plus matching `(offset,length)=>Uint8Array` callbacks
  and `(Uint8Array)=>void` output callback. Returns JSON `{ok:true,receipt}`
  after writing the complete datastream. Errors return `{ok:false,error}`.

Run it in a Web Worker. The output callback must throw on storage failure.
Read callbacks must return exactly the requested bytes from unchanged local
media. Packages currently buffer output within the executor's working-memory
budget; filesystem/disk generation uses the separate streaming interfaces.
