# Provenance

The files in this directory are verbatim, unmodified copies redistributed
under CC BY-ND 4.0 (no derivatives). Do NOT edit them; to update, replace
with a byte-identical copy of a newer upstream release in a new version
directory and regenerate `SHA256SUMS`.

| File | Upstream source |
| --- | --- |
| `core_spec.md` | <https://github.com/aousd/specifications-public/blob/main/core/1.1.0/core_spec.md> |
| `LICENSE` | <https://github.com/aousd/specifications-public/blob/main/LICENSE> (repo root) |

- Upstream repository: `aousd/specifications-public`
- Release tag: `core/v1.1.0`
- Upstream commit for `core/1.1.0/core_spec.md`: `83e25aceb11a9340db4778263bda5d5175875f49` (2026-08-13)
- Document version string: `v1.1.0 (e8f44c0)`
- Verified byte-identical against upstream: 2026-10-27
  (git blob SHAs: `547c4bf23773a35a123d487fc39eaf4131f7953d` for `core_spec.md`,
  `10fbcae9de95e4f34112f2756b0cf4559d472fbc` for `LICENSE`)
- The PDF is not vendored (`aousd/**/*.pdf` is gitignored); fetch it from the
  release tag if needed: `core/1.1.0/core_spec.pdf`.

Verify integrity:

```bash
cd aousd/core/1.1.0 && sha256sum -c SHA256SUMS
```
