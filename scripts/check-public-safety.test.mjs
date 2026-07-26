import assert from "node:assert/strict";
import { execFile } from "node:child_process";
import {
  mkdir,
  mkdtemp,
  rm,
  symlink,
  writeFile,
} from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";

import { scanRepository } from "./check-public-safety.mjs";

const execFileAsync = promisify(execFile);

async function createSandbox(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), "nps-public-safety-"));
  t.after(async () => {
    await rm(root, { force: true, recursive: true });
  });
  await writeFile(
    path.join(root, "package.json"),
    '{"name":"public-safety-sandbox","private":true}\n',
    "utf8",
  );
  return root;
}

async function writeSandboxFile(root, relativePath, contents) {
  const destination = path.join(root, ...relativePath.split("/"));
  await mkdir(path.dirname(destination), { recursive: true });
  await writeFile(destination, contents);
  return destination;
}

function ruleNames(result) {
  return result.violations.map((violation) => violation.rule);
}

test("accepts normal source, pinned Actions, approved emails, and licensed fixtures", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(
    root,
    "README.md",
    [
      "# Safe sandbox",
      "",
      "Contact security@example.com or 123+maintainer@users.noreply.github.com.",
      "A portable example path is /home/user/project.",
      "",
    ].join("\n"),
  );
  await writeSandboxFile(
    root,
    ".github/workflows/ci.yml",
    [
      "name: CI",
      "permissions:",
      "  contents: read",
      "jobs:",
      "  test:",
      "    runs-on: ubuntu-latest",
      "    steps:",
      `      - uses: actions/checkout@${"a".repeat(40)} # pinned`,
      "        with:",
      "          persist-credentials: false",
      "      - uses: ./.github/actions/local",
      `      - { uses: example/action@${"b".repeat(40)} }`,
      "",
    ].join("\n"),
  );
  await writeSandboxFile(
    root,
    "tests/fixtures/public/generated.ppm",
    "P3\n1 1\n255\n0 0 0\n",
  );
  await writeSandboxFile(
    root,
    "tests/fixtures/public/generated.ppm.license.json",
    JSON.stringify(
      {
        license_spdx: "CC0-1.0",
        generated_by: "check-public-safety.test.mjs",
      },
      null,
      2,
    ),
  );

  const result = await scanRepository(root);
  assert.deepEqual(result.violations, []);
});

const forbiddenCases = [
  {
    name: "photo outside the public fixture directory",
    relativePath: "private/photo.JPG",
    contents: "not actually a photo",
    expectedRule: "privacy.media-outside-public-fixtures",
  },
  {
    name: "unlicensed public fixture",
    relativePath: "tests/fixtures/public/photo.png",
    contents: "not actually an image",
    expectedRule: "provenance.fixture-license",
  },
  {
    name: "RAW file even when placed with public fixtures",
    relativePath: "tests/fixtures/public/capture.CR3",
    contents: "raw payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "Photoshop document",
    relativePath: "asset.psd",
    contents: "layered payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "database",
    relativePath: "recovery.sqlite3",
    contents: "database payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "Natural Photo Studio project",
    relativePath: "private-edit.nps",
    contents: "project payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "model",
    relativePath: "weights.onnx",
    contents: "model payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "archive",
    relativePath: "release.zip",
    contents: "archive payload",
    expectedRule: "privacy.forbidden-binary",
  },
  {
    name: "unknown binary",
    relativePath: "payload.dat",
    contents: Buffer.from([0, 1, 2, 3]),
    expectedRule: "privacy.unknown-binary",
  },
];

for (const testCase of forbiddenCases) {
  test(`rejects ${testCase.name}`, async (t) => {
    const root = await createSandbox(t);
    await writeSandboxFile(
      root,
      testCase.relativePath,
      testCase.contents,
    );

    const result = await scanRepository(root);
    assert.ok(
      ruleNames(result).includes(testCase.expectedRule),
      JSON.stringify(result.violations, null, 2),
    );
  });
}

test("rejects high-confidence credentials and private keys", async (t) => {
  const root = await createSandbox(t);
  const token = ["ghp", "_", "A".repeat(40)].join("");
  const privateKeyHeader = ["-----BEGIN ", "PRIVATE KEY-----"].join("");
  await writeSandboxFile(
    root,
    "secrets.txt",
    `${token}\n${privateKeyHeader}\n`,
  );

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("secret.github-token"));
  assert.ok(ruleNames(result).includes("secret.private-key"));
});

test("rejects high-entropy credentials assigned to sensitive fields", async (t) => {
  const root = await createSandbox(t);
  const field = ["api", "_key"].join("");
  const value = ["m9Qx", "2pVr", "7LkD", "4sNa", "8BcW", "6tYz"].join("");
  await writeSandboxFile(root, "config.txt", `${field} = "${value}"\n`);

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("secret.sensitive-assignment"));
});

test("rejects personal email addresses", async (t) => {
  const root = await createSandbox(t);
  const personalAddress = ["person", "@", "mail", ".", "invalidtest"].join("");
  await writeSandboxFile(root, "contact.md", `${personalAddress}\n`);

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("privacy.personal-email"));
});

test("rejects absolute Windows and POSIX user paths", async (t) => {
  const root = await createSandbox(t);
  const windowsPath = ["C:", "\\", "Users", "\\", "Alice", "\\", "project"].join(
    "",
  );
  const nonProfileWindowsPath =
    ["D:", "\\", "work", "\\", "client", "\\", "project"].join("");
  const uncPath =
    ["\\\\", "private-server", "\\", "client-share", "\\", "project"].join("");
  const escapedWindowsPath = JSON.stringify({ cachePath: windowsPath });
  const posixPath = ["/home/", "alice", "/project"].join("");
  await writeSandboxFile(
    root,
    "paths.md",
    [
      windowsPath,
      nonProfileWindowsPath,
      uncPath,
      escapedWindowsPath,
      posixPath,
      "",
    ].join("\n"),
  );

  const result = await scanRepository(root);
  assert.equal(
    result.violations.filter(
      (violation) => violation.rule === "privacy.absolute-user-path",
    ).length,
    5,
  );
});

test("rejects GitHub Actions that use a mutable ref", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(
    root,
    ".github/workflows/ci.yml",
    [
      "permissions:",
      "  contents: read",
      "jobs:",
      "  test:",
      "    runs-on: ubuntu-latest",
      "    steps:",
      "      - uses: actions/checkout@v4",
      '      - "uses": actions/setup-node@v4',
      "      - { uses: example/action@main }",
      "",
    ].join("\n"),
  );

  const result = await scanRepository(root);
  assert.equal(
    result.violations.filter(
      (violation) =>
        violation.rule === "supply-chain.action-not-pinned",
    ).length,
    3,
  );
});

test("rejects unsafe public-repository workflow capabilities", async (t) => {
  const root = await createSandbox(t);
  const privilegedTrigger = ["pull_request", "_target:"].join("");
  const inheritedSecrets = ["secrets:", " inherit"].join("");
  await writeSandboxFile(
    root,
    ".github/workflows/unsafe.yml",
    [
      "on:",
      `  ${privilegedTrigger}`,
      "permissions: write-all",
      "jobs:",
      "  test:",
      "    runs-on: [self-hosted, windows]",
      `    ${inheritedSecrets}`,
      "    container: example.invalid/tool:latest",
      "    steps:",
      `      - uses: actions/checkout@${"a".repeat(40)}`,
      "",
    ].join("\n"),
  );

  const result = await scanRepository(root);
  for (const expectedRule of [
    "workflow.privileged-pr-trigger",
    "workflow.broad-permissions",
    "workflow.inherited-secrets",
    "workflow.self-hosted-runner",
    "supply-chain.container-not-pinned",
    "workflow.checkout-credentials-not-disabled",
  ]) {
    assert.ok(
      ruleNames(result).includes(expectedRule),
      `${expectedRule}: ${JSON.stringify(result.violations, null, 2)}`,
    );
  }
});

test("requires explicit top-level workflow permissions", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(
    root,
    ".github/workflows/no-permissions.yml",
    [
      "jobs:",
      "  test:",
      "    runs-on: ubuntu-24.04",
      "    steps:",
      "      - run: npm test",
      "",
    ].join("\n"),
  );

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("workflow.permissions-missing"));
});

test("rejects files above the configured size limit", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(root, "large.txt", "x".repeat(128));

  const result = await scanRepository(root, { maxFileBytes: 64 });
  assert.ok(ruleNames(result).includes("filesystem.large-file"));
});

test("rejects invalid fixture license metadata", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(
    root,
    "tests/fixtures/public/photo.png",
    "not actually an image",
  );
  await writeSandboxFile(
    root,
    "tests/fixtures/public/photo.png.license.json",
    JSON.stringify({
      license_spdx: "not-a-license",
      source: true,
    }),
  );

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("provenance.fixture-license"));
});

test("scans nested directories even when their name is excluded at the root", async (t) => {
  const root = await createSandbox(t);
  await writeSandboxFile(
    root,
    "docs/build/private.jpg",
    "private media hidden in a nested build directory",
  );
  await writeSandboxFile(
    root,
    "examples/node_modules/private.jpg",
    "private media hidden in a nested dependency directory",
  );

  const result = await scanRepository(root);
  assert.equal(
    result.violations.filter(
      (violation) =>
        violation.rule === "privacy.media-outside-public-fixtures",
    ).length,
    2,
  );
});

test("rejects forced tracked files in excluded root directories", async (t) => {
  const root = await createSandbox(t);
  await execFileAsync("git", ["-C", root, "init", "--quiet"]);
  await writeSandboxFile(
    root,
    "build/private.jpg",
    "private media forced into an ignored root",
  );
  await execFileAsync(
    "git",
    ["-C", root, "add", "--force", "--", "build/private.jpg"],
  );

  const result = await scanRepository(root);
  assert.ok(
    ruleNames(result).includes("repository.tracked-excluded-path"),
    JSON.stringify(result.violations, null, 2),
  );
});

test("rejects symbolic links", async (t) => {
  const root = await createSandbox(t);
  const target = await writeSandboxFile(root, "target.txt", "safe\n");
  const link = path.join(root, "linked.txt");

  try {
    await symlink(target, link, "file");
  } catch (error) {
    if (error?.code === "EPERM" || error?.code === "EACCES") {
      const targetDirectory = path.join(root, "target-directory");
      const junction = path.join(root, "linked-directory");
      await mkdir(targetDirectory);
      try {
        await symlink(targetDirectory, junction, "junction");
      } catch (junctionError) {
        if (
          junctionError?.code === "EPERM" ||
          junctionError?.code === "EACCES"
        ) {
          t.skip(
            "This Windows environment permits neither file links nor directory junctions.",
          );
          return;
        }
        throw junctionError;
      }
    } else {
      throw error;
    }
  }

  const result = await scanRepository(root);
  assert.ok(ruleNames(result).includes("filesystem.symlink"));
});

test("does not walk above the explicitly supplied root", async (t) => {
  const parent = await mkdtemp(path.join(os.tmpdir(), "nps-root-boundary-"));
  t.after(async () => {
    await rm(parent, { force: true, recursive: true });
  });
  const root = path.join(parent, "repository");
  await mkdir(root);
  await writeSandboxFile(root, "README.md", "# Repository\n");
  await writeSandboxFile(parent, "private.jpg", "outside scan root");

  const result = await scanRepository(root);
  assert.deepEqual(result.violations, []);
  assert.equal(result.filesChecked, 1);
});
