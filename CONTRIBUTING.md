# Contributing

Natural Photo Studio welcomes documentation, tests, image-engine, UX, accessibility, privacy, AI evaluation, and tooling contributions.

## Before opening a pull request

1. Create a focused branch.
2. Reference a requirement ID or explain which documented invariant the change advances.
3. Run `npm ci --ignore-scripts` and `npm run check`.
4. For C++ or build changes, run `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`.
5. Add or update tests.
6. Update documentation when behavior or a public contract changes.
7. Confirm the diff contains no private material.

## Privacy rules

Never commit:

- personal or customer photographs;
- RAW, PSD, PSB, TIFF, NPS projects, thumbnails, masks, or exports;
- EXIF/GPS captured from private images;
- face landmarks, embeddings, identity labels, or biometric caches;
- API keys, cookies, tokens, certificates, credentials, or private URLs;
- absolute local paths, user names, emails, phone numbers, or client names;
- model weights or datasets without a reviewed manifest and license.

Tests must generate pixels in memory or use a fixture in `tests/fixtures/public/` with an adjacent license manifest. A fixture is not public merely because it was found online.

## Engineering rules

- Original sources are immutable.
- Permanent document changes go through the command protocol.
- Preview operations cannot silently commit.
- Cache deletion cannot change final results.
- Natural-language and plugin paths cannot execute arbitrary code or gain implicit file/network access.
- New model or plugin artifacts require provenance, license, hash, evaluation, and rollback metadata.

## Pull request content

Explain:

- what changed and why;
- user/developer impact;
- requirement or ADR relationship;
- tests and platforms used;
- privacy/security effects;
- migration or compatibility effects.

Large design changes to project format, command schemas, color semantics, cloud policy, or plugin permissions require an ADR.

## License

By contributing, you agree that your contributions are licensed under the repository's Apache License 2.0.
