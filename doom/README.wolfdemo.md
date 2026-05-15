# wolfDemo Doom Engine Snapshot

This directory is a minimal vendored snapshot of the Doom engine sources
used by the wolfDemo STM32U585 firmware.

It was imported from:

- Repository: `https://github.com/LinuxJedi/rp2040-doom.git`
- Branch: `wolfdemo`
- Commit: `3239878b1045249467e5f2a4bcfb5cc167c2dcda`

That branch is a fork of `https://github.com/kilograham/rp2040-doom`,
which in turn derives from Chocolate Doom / id Software Doom sources.

Only files reachable by the firmware build dependency graph are kept
here, plus the upstream license and attribution documents. This is no
longer a Git submodule; changes under `doom/` are committed directly in
the wolfDemo firmware repository.
