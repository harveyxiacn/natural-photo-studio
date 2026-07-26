#!/usr/bin/env node

import { readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const PROJECT_ROOT = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
);
const FULL_SHA256 = /^[0-9a-f]{64}$/u;
const FULL_GIT_COMMIT = /^[0-9a-f]{40}$/u;
const EXACT_SEMVER = /^\d+\.\d+\.\d+$/u;

async function readProjectFile(relativePath) {
  return readFile(path.join(PROJECT_ROOT, relativePath), "utf8");
}

async function readJson(relativePath) {
  return JSON.parse(await readProjectFile(relativePath));
}

function collectYamlScalar(text, key) {
  const pattern = new RegExp(
    `^\\s*${key}\\s*:\\s*["']?([^"'#\\s]+)["']?\\s*(?:#.*)?$`,
    "gmu",
  );
  return [...text.matchAll(pattern)].map((match) => match[1]);
}

function addViolation(violations, file, message) {
  violations.push({ file, message });
}

async function main() {
  const violations = [];
  const packageJson = await readJson("package.json");
  const packageLock = await readJson("package-lock.json");
  const vcpkg = await readJson("vcpkg.json");
  const workflows = new Map([
    [".github/workflows/ci.yml", await readProjectFile(".github/workflows/ci.yml")],
    [
      ".github/workflows/codeql.yml",
      await readProjectFile(".github/workflows/codeql.yml"),
    ],
  ]);

  if (packageJson.private !== true) {
    addViolation(
      violations,
      "package.json",
      'the policy-only npm package must retain `"private": true`',
    );
  }

  const packageManagerMatch =
    typeof packageJson.packageManager === "string"
      ? packageJson.packageManager.match(/^npm@(.+)$/u)
      : null;
  if (!packageManagerMatch || !EXACT_SEMVER.test(packageManagerMatch[1])) {
    addViolation(
      violations,
      "package.json",
      "packageManager must pin npm to an exact semantic version",
    );
  }

  if (
    packageLock.lockfileVersion !== 3 ||
    packageLock.name !== packageJson.name ||
    packageLock.version !== packageJson.version ||
    packageLock.packages?.[""]?.name !== packageJson.name ||
    packageLock.packages?.[""]?.version !== packageJson.version
  ) {
    addViolation(
      violations,
      "package-lock.json",
      "root package identity/version must match package.json and lockfile v3",
    );
  }

  const baseline = vcpkg["builtin-baseline"];
  if (typeof baseline !== "string" || !FULL_GIT_COMMIT.test(baseline)) {
    addViolation(
      violations,
      "vcpkg.json",
      "builtin-baseline must be a full lowercase Git commit",
    );
  }

  for (const [workflowPath, workflowText] of workflows) {
    const workflowCommits = collectYamlScalar(workflowText, "VCPKG_COMMIT");
    if (
      workflowCommits.length !== 1 ||
      workflowCommits[0] !== baseline
    ) {
      addViolation(
        violations,
        workflowPath,
        "VCPKG_COMMIT must occur once and equal vcpkg.json builtin-baseline",
      );
    }

    if (
      /\bruns-on\s*:\s*(?:ubuntu|windows|macos)-latest\b/iu.test(
        workflowText,
      )
    ) {
      addViolation(
        violations,
        workflowPath,
        "runner operating-system labels must name an explicit version",
      );
    }
  }

  const ciText = workflows.get(".github/workflows/ci.yml");
  const nodeVersions = collectYamlScalar(ciText, "node-version");
  if (
    nodeVersions.length !== 1 ||
    !EXACT_SEMVER.test(nodeVersions[0])
  ) {
    addViolation(
      violations,
      ".github/workflows/ci.yml",
      "the policy job must pin Node.js to one exact semantic version",
    );
  }

  const gitleaksVersions = collectYamlScalar(ciText, "GITLEAKS_VERSION");
  const gitleaksChecksums = collectYamlScalar(
    ciText,
    "GITLEAKS_LINUX_X64_SHA256",
  );
  if (
    gitleaksVersions.length !== 1 ||
    !EXACT_SEMVER.test(gitleaksVersions[0]) ||
    gitleaksChecksums.length !== 1 ||
    !FULL_SHA256.test(gitleaksChecksums[0])
  ) {
    addViolation(
      violations,
      ".github/workflows/ci.yml",
      "Gitleaks must use one exact version and one lowercase SHA-256 checksum",
    );
  }
  const gitleaksConfigReferences = [
    ...ciText.matchAll(
      /--config\s+scripts\/gitleaks-public\.toml\b/gu,
    ),
  ];
  if (gitleaksConfigReferences.length !== 2) {
    addViolation(
      violations,
      ".github/workflows/ci.yml",
      "Gitleaks working-tree and history scans must share the reviewed public config",
    );
  }

  for (const requiredRunner of [
    "ubuntu-24.04",
    "windows-2022",
    "macos-15",
  ]) {
    if (!ciText.includes(`os: ${requiredRunner}`)) {
      addViolation(
        violations,
        ".github/workflows/ci.yml",
        `the core matrix is missing ${requiredRunner}`,
      );
    }
  }

  if (violations.length > 0) {
    console.error(
      `Supply-chain policy failed with ${violations.length} violation(s):`,
    );
    for (const violation of violations) {
      console.error(`- ${violation.file}: ${violation.message}`);
    }
    process.exitCode = 1;
    return;
  }

  console.log(
    "Supply-chain policy passed (npm, vcpkg, runner, Node.js, and Gitleaks pins agree).",
  );
}

main().catch((error) => {
  console.error(`Supply-chain policy could not run: ${error.message}`);
  process.exitCode = 1;
});
