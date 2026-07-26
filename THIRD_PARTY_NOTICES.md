# Third-party notices

Natural Photo Studio M0 uses the following packages at the baseline recorded in
`vcpkg.json`. The versions below are the versions resolved from that baseline
for the local M0 verification build.

| Component | Version | Use | Upstream license |
|---|---:|---|---|
| nlohmann/json | 3.12.0 | Command JSON parsing and serialization | MIT |
| OpenSSL | 3.6.3 | SHA-256 and cryptographically secure identifiers | Apache-2.0 |
| SQLite | 3.53.3 | Project metadata, history, and recovery journal | Public domain |
| Catch2 | 3.15.2 | Test framework; not linked into the application CLI | BSL-1.0 |

Node.js 24 or later is used to run repository policy checks. CMake, Ninja, npm,
and vcpkg are build tools and are not redistributed by this repository.
Gitleaks 8.30.1 (MIT) is downloaded by CI from its official release, verified
against a pinned SHA-256 digest, and used only to scan Git history.

No end-user application package is published at M0. The CMake install target
creates a developer/CI staging tree and, on Windows, copies the vcpkg runtime
DLLs needed by the CLI; CI does not upload that tree as a release artifact.
Before distributing a binary package, release engineering must include the
exact upstream copyright and license texts required by the resolved artifacts,
verify static versus dynamic linkage, and regenerate this notice from the
release dependency lock.

GitHub Actions are not redistributed with the application. Workflow references
are pinned to full commit hashes; their version labels are recorded beside each
reference.
