# Contributing to Himmel

Thank you! Issues, ideas, hardware reports and pull requests are all welcome.

## Good first contributions

- **Run it on your MK2** and tell us what happened: the live view
  (`python tools/mk2_live.py`) shows if your unit matches the map. Differences
  are valuable: open an issue with the re-learned control.
- **Measure.** Latency, frame times, decode times on your GPU. The renderer
  logs them every 5 s.
- **A new screen mode** for the MK2 screens: the recipe is in
  [docs/mk2-screen-fun.md](docs/mk2-screen-fun.md).
- **Docs**: anything that confused you in the getting-started guide.

## Rules of the house

These come from the project goals (show-safe, low latency, easy, light on the GPU):

1. **The frame loop never allocates, locks or touches the disk.** Do loading
   in a worker and hand results over lock-free.
2. **Every part must survive another part dying.** Use heartbeats; never block
   on another process or thread.
3. **Measure instead of guessing**, and test on real hardware when hardware is
   involved. Say in the PR what you tested on.
4. **Tests:** `mk2_tests.exe` and `media_tests.exe` must pass. A bug found on
   hardware gets a unit test when it can be reproduced without hardware.
5. **Code, comments, logs and UI text are in English.** C++20, `/W4` clean,
   no new warnings.
6. **New dependencies:** check the license first and add it to
   [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) in the same PR. Big or
   copyleft dependencies (GPL, proprietary SDKs) need a discussion first.
   Copyleft code goes in a separate process ("bridge"), so the core stays MIT.
7. **Credit your sources.** If you learned a protocol detail or an algorithm
   from somewhere, link it in the notices.

## AI-assisted contributions

Welcome: this project is AI-assisted itself (see
[docs/ai-assisted-development.md](docs/ai-assisted-development.md)). Please:

- say in the PR that an AI helped, and which one;
- make sure you understand and have run what you submit;
- keep the `Co-Authored-By:` trailer your tool adds, or add one.

## License of contributions

By contributing you agree that your contribution is licensed under the
project's [MIT license](LICENSE).
