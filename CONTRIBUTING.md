# Contributing to the PulseCore Game Integration SDK

Thanks for helping build the PulseCore modding ecosystem. This repo is small on purpose — the protocol is
meant to stay stable — so most contributions are new examples, rule packs, docs, and language bindings.

## Ways to contribute

- **New example integrations** for other games/engines (like the Half-Life 2 mod).
- **Rule packs** — `effects.json` mappings from your game's events to trigger effects.
- **Language bindings** for the C ABI (Rust, C#, Python, …).
- **Docs and fixes.**

## Ground rules

- **Don't break Protocol v1.** Add fields compatibly; never make an unknown `type`/`name` a hard error.
  Anything genuinely breaking is a new version (and a new pipe-name suffix), discussed in an issue first.
- **Keep the proprietary side out.** This repo is only the open protocol + client SDK + examples. The
  effects engine, pipe server, and DualSense/USB-IP bridge are not part of it and must not be reproduced
  or reverse-engineered here.
- **Windows-first.** The SDK uses a Windows named pipe (PulseCore is Windows-only). `pulse-protocol` is
  portable; keep it that way (no OS headers).
- **Style.** Match the surrounding code. C++20, no exceptions across the ABI, small and dependency-free.

## How to submit

1. Open an issue describing the change (especially for anything protocol-adjacent).
2. Fork, branch, and keep the change focused.
3. Make sure `cmake -B build -S . && cmake --build build` succeeds on Windows.
4. Open a pull request.

## Licensing of contributions

By submitting a contribution you agree that:

1. Your contribution is licensed to the project and its users under **PolyForm Noncommercial 1.0.0**
   (the repository license); and
2. so the project can continue to offer **commercial licenses** to companies that need them, you also
   grant the maintainer a perpetual, irrevocable, worldwide right to license your contribution under
   other terms (including commercial ones), and you confirm you have the right to grant this (it's your
   own work, or you have permission).

This "inbound = outbound, plus a commercial grant" model is what keeps the SDK both free for the
community and viable as a product. If you can't grant #2, say so in the PR and we'll figure it out.

You retain copyright to your contributions.
