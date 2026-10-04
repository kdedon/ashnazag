# Hosting the Ash Nazag forge

The public repository is [kdedon/ashnazag](https://github.com/kdedon/ashnazag).
GitHub Pages can serve the static HTML, JavaScript and Go WASM at
`https://kdedon.github.io/ashnazag/`. That is the intended address; deployment
has not been verified. The intended browser image engine will run on the visitor's computer; the browser now builds a
complete Quadra console image from local tape segments, a donor disk and a prebuilt
kernel. Guest/package and other machine recipes remain planned.

## First deployment

1. Regenerate the public export with `sh .publish.sh` from the private workspace.
   Review `git -C publish diff` and the exported file list, then commit and push
   the reviewed export to the public repository's default branch.
2. In the public repository, select **Settings → Pages → Source → GitHub Actions**.
3. Protect the `github-pages` environment so only the default branch can deploy.
4. Push to the default branch, or use **Actions → Ash Nazag image forge → Run
   workflow** on it.
5. Open the deployment URL and check WASM startup and media selection in supported
   browsers. Check advanced prepared-image assembly and download with local fixtures.

The workflow builds and tests pull requests and pushes with read-only repository
permissions. Every push to the public repository's default branch that changes
the site deploys it once the tests pass; pull requests never deploy. Only the deployment job receives Pages and OIDC
write permissions. CI uses synthetic fixtures; local installation media, ROMs
and generated system images stay outside the published site.

Relative asset URLs preserve the repository subpath. No backend, PAT or custom
domain is required. The build copies the Go runtime matching its WASM compiler.
See GitHub's [custom workflow instructions](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)
and [deployment branch protection guidance](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site).

## Public build artifacts

Ship small, redistributable packs alongside the site so browser downloads remain
same-origin. Keep each released pack immutable under a versioned path, for example
`artifacts/v1/m68k-68040/ashnazag-abi-1/<sha256>.pack`. The versioned catalog must
record:

- SHA-256, byte length and format version.
- CPU requirements, ABI, target machines and module dependencies.
- Source commit, toolchain version and reproducible build recipe.
- License and provenance for every included component.

The loader must validate size, hash and compatibility before using a pack.
Pin each build to a catalog version and record its exact pack hashes in the local
build report. Publish a new catalog version when any pack changes. A digest
detects corruption; provenance review determines whether an artifact can be
distributed.

This catalog and pack publisher remain future work. The current workflow publishes
only `webapp/build/site`. Establish redistribution rights before adding kernel
objects or third-party packages to it. User-supplied AMIX, A/UX and ROM inputs stay
local; their presence in a native build does not make them public artifacts.

Keep Pages for the application and modest artifact packs. GitHub limits published
sites to 1 GB; larger packs need separate hosting with browser-compatible CORS,
streaming and range-request behavior verified before enabling remote fetching.
Release assets can distribute native tools, but browser fetch support must be
tested rather than assumed. See [Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits).

## Browser execution

Use a dedicated worker and transferable byte buffers. Avoid requiring shared-memory
WASM threads for the Pages baseline. Stream large outputs into browser storage
where available and offer a bounded-memory fallback. Explicit browser support and
storage limits should precede production image generation; hosting the UI alone
does not prove that a generated image boots.
