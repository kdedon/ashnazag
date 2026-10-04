# Source importers

`Read(ctx, format, reader, size, limits)` returns a bounded file tree without
extracting anything onto the host. Formats are `cpio`, `svr4`, `lha`, `adf`, and
classic `hfs`. Input and expanded data default to 256 MiB, with 100,000 entries.
Paths, sizes, collisions and format-specific checksums are validated. Cancellation
propagates through readers and decoding. Callers keep source readers unchanged.

LHA supports header levels 0–2 and stored/LH4/LH5/LH6/LH7 methods. The internal
MIT-licensed decoder has its own LICENSE and provenance notice. Nameless entries,
unsupported compression and malformed headers fail. The local Picasso96 archive
has two nameless entries: its valid prefix is tested against an independent
extractor, while the whole archive is rejected.

ADF supports DD/HD OFS/FFS DOS0–3 filesystems. Protection bits, comments and
Amiga timestamps are retained. Package execution rejects attributes whose target
host persistence has not been qualified.

HFS supports raw volumes or one HFS partition in an Apple partition map,
fragmented forks and bounded catalog/extent traversal. Finder information and
resource forks become A/UX AppleDouble `%` companions. Raw MacRoman name bytes
remain intact in the imported tree; package receipts currently require UTF-8,
so unsupported name conversion fails during package execution. HFS+ and ambiguous
multi-HFS images fail explicitly.
