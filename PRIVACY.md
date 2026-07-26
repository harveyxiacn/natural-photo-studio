# Privacy

Natural Photo Studio is being designed as a local-first, non-destructive photo editor. Privacy is a release requirement, not an optional feature. This document separates rules enforced by the current public source repository from capabilities planned for the finished application.

## Public repository rules

Real user media must never be committed to this repository. That includes photos, video, audio, camera RAW files, layered editor documents, project databases, recovery files, trained model weights, and archives that might conceal any of those items.

Small media fixtures are permitted only under `tests/fixtures/public/`. Every fixture must be generated specifically for testing or have redistribution rights that are independently verified. It must have an adjacent `LICENSE` containing `SPDX-License-Identifier:` and `Source:` fields, or a `.license.json` sidecar containing an approved `license_spdx` value and a non-placeholder provenance string. The repository scanner intentionally accepts only the reviewed fixture-license allowlist; expanding it requires a policy and test change. Camera RAW files, layered editor documents, databases, model weights, compiled binaries, and archives are prohibited even in the fixture directory.

Automated repository checks reject:

- media outside the public fixture directory;
- unlicensed public fixtures and prohibited binary formats;
- common high-confidence credentials and private keys;
- personal email addresses and absolute user-home paths;
- oversized files, symbolic links, and unrecognized binary payloads; and
- GitHub Actions dependencies that are not pinned to immutable commit SHAs.

Contributors should still inspect staged changes manually. Automated detection reduces risk but cannot prove that a file is anonymous, properly licensed, or free of confidential context.

## Application privacy goals

The planned desktop application follows these defaults:

- Editing, indexing, previews, undo history, and supported AI inference run locally.
- Telemetry is off unless a user makes a separate, informed choice to enable it.
- A cloud-backed operation requires an explicit preview of what will leave the device, the selected provider, and the applicable retention policy.
- Metadata export is controllable. Location, device serial numbers, edit history, and identity-related metadata are removed by default from share-oriented exports.
- Temporary files, caches, and crash-recovery data have documented locations and deletion controls.
- Network access is denied by default for plug-ins and models unless a narrowly scoped capability is granted.

These are design requirements under active implementation. They must not be interpreted as claims about an unfinished build.

## Reports and support requests

Do not attach private photos, original project files, access tokens, logs containing home-directory paths, or full metadata dumps to a public issue. Reproduce a problem with a generated fixture whenever possible.

For a security or privacy vulnerability, use the private reporting process described in [SECURITY.md](SECURITY.md). For an accidental public disclosure, contact the maintainers through the repository's private security-reporting interface and identify the affected path or issue. Do not repeat the sensitive value in additional comments.

## Data subject and licensing responsibilities

People depicted in test or demonstration media must have consented to the intended public use, and the contributor must have the right to redistribute that media. A permissive file license does not replace model releases, privacy consent, trademark clearance, or local legal requirements.

Generated fixtures should avoid recognizable real people, private residences, unique documents, vehicle plates, and other identifying details. Synthetic provenance must also be recorded; “AI-generated” does not by itself establish unrestricted rights.

## Changes to this policy

Privacy-affecting changes should update this document, the threat model, and the relevant automated checks in the same pull request. A relaxation of a repository gate requires a written rationale and maintainer review.
