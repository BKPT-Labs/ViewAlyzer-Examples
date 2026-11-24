# ViewAlyzer examples

For BKPT work, read the shared coding and review agreement in the workspace
root (`../AGENTS.md` in the standard layout). In a separate worktree, use the
BKPT workspace location in your personal instructions.

Start with the example closest in board, runtime, and transport. Follow that
project's README and build entry point. The root `README.md` documents recorder
discovery and per-machine tool paths. Keep committed example settings portable.
Preserve the examples' licensing and the notices on bundled vendor sources.

Use `AI_INTEGRATION.md` for recorder configuration, transport selection, and
capture. For domain examples, also read `domains/README.md`. Build the affected
example using its documented toolchain. For a capture check, use that example's
transport settings and matching ELF where required, then check the recording
for the events the example promises. Keep local tool settings, build products,
and captures outside the example change.
