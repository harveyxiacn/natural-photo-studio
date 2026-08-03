#!/usr/bin/env node

import { execFile } from "node:child_process";
import { lstat, readdir, readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath, pathToFileURL } from "node:url";
import { promisify, TextDecoder } from "node:util";

export const DEFAULT_MAX_FILE_BYTES = 5 * 1024 * 1024;

const EXCLUDED_DIRECTORIES = new Set([
  ".git",
  "node_modules",
  "build",
  ".tools",
  ".tmp",
]);

const PUBLIC_FIXTURE_ROOT = "tests/fixtures/public";
const APPROVED_PUBLIC_FIXTURE_LICENSES = new Set([
  "0BSD",
  "Apache-2.0",
  "BSD-2-Clause",
  "BSD-3-Clause",
  "CC-BY-4.0",
  "CC-BY-SA-4.0",
  "CC0-1.0",
  "MIT",
  "Unlicense",
]);
const PROJECT_ROOT = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
);
const execFileAsync = promisify(execFile);

const MEDIA_EXTENSIONS = new Set([
  ".avif",
  ".bmp",
  ".exr",
  ".gif",
  ".hdr",
  ".heic",
  ".heif",
  ".ico",
  ".jpeg",
  ".jpg",
  ".m4a",
  ".mov",
  ".mp3",
  ".mp4",
  ".pbm",
  ".pdf",
  ".pgm",
  ".png",
  ".ppm",
  ".tif",
  ".tiff",
  ".wav",
  ".webm",
  ".webp",
]);

const ALWAYS_FORBIDDEN_EXTENSIONS = new Map([
  [
    "camera RAW or layered editor document",
    new Set([
      ".3fr",
      ".arw",
      ".cr2",
      ".cr3",
      ".dng",
      ".erf",
      ".iiq",
      ".kdc",
      ".mef",
      ".mos",
      ".mrw",
      ".nef",
      ".nrw",
      ".orf",
      ".pef",
      ".psb",
      ".psd",
      ".raf",
      ".raw",
      ".rw2",
      ".rwl",
      ".sr2",
      ".srw",
      ".x3f",
      ".xcf",
    ]),
  ],
  [
    "database or application project",
    new Set([
      ".db",
      ".db3",
      ".mdb",
      ".nps",
      ".npsproj",
      ".sqlite",
      ".sqlite3",
    ]),
  ],
  [
    "model, checkpoint, or compiled binary",
    new Set([
      ".bin",
      ".ckpt",
      ".dll",
      ".dylib",
      ".engine",
      ".exe",
      ".ggml",
      ".gguf",
      ".onnx",
      ".pt",
      ".pth",
      ".safetensors",
      ".so",
      ".tflite",
    ]),
  ],
  [
    "archive or compressed payload",
    new Set([
      ".7z",
      ".bz2",
      ".cab",
      ".gz",
      ".iso",
      ".lz",
      ".lz4",
      ".lzma",
      ".rar",
      ".tar",
      ".tgz",
      ".txz",
      ".xz",
      ".zip",
      ".zst",
    ]),
  ],
]);

const SECRET_PATTERNS = [
  {
    rule: "secret.private-key",
    message: "private key material",
    pattern: /-----BEGIN (?:DSA |EC |OPENSSH |PGP |RSA )?PRIVATE KEY-----/,
  },
  {
    rule: "secret.github-token",
    message: "GitHub access token",
    pattern: /\b(?:gh[pousr]_[A-Za-z0-9]{36,255}|github_pat_[A-Za-z0-9_]{60,255})\b/,
  },
  {
    rule: "secret.aws-access-key",
    message: "AWS access key identifier",
    pattern: /\b(?:AKIA|ASIA)[A-Z0-9]{16}\b/,
  },
  {
    rule: "secret.openai-key",
    message: "OpenAI API key",
    pattern: /\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}\b/,
  },
  {
    rule: "secret.google-api-key",
    message: "Google API key",
    pattern: /\bAIza[0-9A-Za-z_-]{35}\b/,
  },
  {
    rule: "secret.slack-token",
    message: "Slack token",
    pattern: /\bxox[baprs]-[A-Za-z0-9-]{20,}\b/,
  },
  {
    rule: "secret.npm-token",
    message: "npm access token",
    pattern: /\bnpm_[A-Za-z0-9]{36,}\b/,
  },
  {
    rule: "secret.gitlab-token",
    message: "GitLab access token",
    pattern: /\bglpat-[A-Za-z0-9_-]{20,}\b/,
  },
  {
    rule: "secret.stripe-key",
    message: "live Stripe secret key",
    pattern: /\bsk_live_[A-Za-z0-9]{20,}\b/,
  },
  {
    rule: "secret.jwt",
    message: "JSON Web Token",
    pattern: /\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b/,
  },
];

const SENSITIVE_ASSIGNMENT =
  /\b(?:api[_-]?key|access[_-]?token|auth[_-]?token|client[_-]?secret|password|passwd|private[_-]?key)\b\s*[:=]\s*["'`]([^"'`\s]{20,})["'`]/giu;

const EMAIL_PATTERN =
  /\b[A-Z0-9.!#$%&'*+/=?^_`{|}~-]+@(?:[A-Z0-9](?:[A-Z0-9-]{0,61}[A-Z0-9])?\.)+[A-Z]{2,63}\b/giu;

const WINDOWS_ABSOLUTE_PATH_PATTERN =
  /(?:^|[\s("'`])([A-Za-z]:(?:\\{1,2}|\/))/gmu;

const WINDOWS_UNC_PATH_PATTERN =
  /(?:^|[\s("'`])(\\{2,4}[A-Za-z0-9._-]+\\{1,2}[A-Za-z0-9$._ -]+)/gmu;

const POSIX_USER_PATH_PATTERN =
  /(?<![:A-Za-z0-9])\/(?:Users|home)\/([A-Za-z0-9._-]+)/gu;

const PLACEHOLDER_USER_NAMES = new Set([
  "example",
  "runner",
  "sandbox",
  "user",
  "username",
]);

const UTF8_DECODER = new TextDecoder("utf-8", { fatal: true });

function normalizeRelative(relativePath) {
  return relativePath.split(path.sep).join("/");
}

function isExcludedRootDirectory(relativePath) {
  return (
    !relativePath.includes("/") &&
    EXCLUDED_DIRECTORIES.has(relativePath.toLowerCase())
  );
}

function isInsidePublicFixtures(relativePath) {
  const lower = relativePath.toLowerCase();
  return (
    lower === PUBLIC_FIXTURE_ROOT ||
    lower.startsWith(`${PUBLIC_FIXTURE_ROOT}/`)
  );
}

function extensionCategory(extension) {
  for (const [category, extensions] of ALWAYS_FORBIDDEN_EXTENSIONS) {
    if (extensions.has(extension)) {
      return category;
    }
  }
  return null;
}

function lineNumberAt(text, index) {
  let line = 1;
  for (let position = 0; position < index; position += 1) {
    if (text.charCodeAt(position) === 10) {
      line += 1;
    }
  }
  return line;
}

function entropy(value) {
  const frequencies = new Map();
  for (const character of value) {
    frequencies.set(character, (frequencies.get(character) ?? 0) + 1);
  }

  let result = 0;
  for (const count of frequencies.values()) {
    const probability = count / value.length;
    result -= probability * Math.log2(probability);
  }
  return result;
}

function looksLikePlaceholder(value) {
  return /(?:change[-_]?me|dummy|example|fake|placeholder|redacted|sample|test|your[-_])/iu.test(
    value,
  );
}

function isMeaningfulProvenance(value) {
  return (
    typeof value === "string" &&
    value.trim().length >= 3 &&
    !/^(?:n\/a|none|tbd|todo|true|unknown)$/iu.test(value.trim())
  );
}

function isAllowedEmail(email) {
  const domain = email.slice(email.lastIndexOf("@") + 1).toLowerCase();
  return (
    domain === "example.com" ||
    domain === "example.net" ||
    domain === "example.org" ||
    domain.endsWith(".example") ||
    domain === "noreply.github.com" ||
    domain.endsWith(".noreply.github.com")
  );
}

function findSecretViolations(text) {
  const violations = [];

  for (const definition of SECRET_PATTERNS) {
    const match = definition.pattern.exec(text);
    if (match) {
      violations.push({
        rule: definition.rule,
        line: lineNumberAt(text, match.index),
        message: `possible ${definition.message}`,
      });
    }
  }

  SENSITIVE_ASSIGNMENT.lastIndex = 0;
  for (const match of text.matchAll(SENSITIVE_ASSIGNMENT)) {
    const value = match[1];
    if (
      !looksLikePlaceholder(value) &&
      /[A-Za-z]/u.test(value) &&
      /[0-9]/u.test(value) &&
      entropy(value) >= 3.5
    ) {
      violations.push({
        rule: "secret.sensitive-assignment",
        line: lineNumberAt(text, match.index),
        message: "possible high-entropy credential assigned to a sensitive field",
      });
    }
  }

  return violations;
}

function findEmailViolations(text) {
  const violations = [];
  EMAIL_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(EMAIL_PATTERN)) {
    if (!isAllowedEmail(match[0])) {
      violations.push({
        rule: "privacy.personal-email",
        line: lineNumberAt(text, match.index),
        message: "personal or organization email address; use example or GitHub noreply",
      });
    }
  }
  return violations;
}

function findAbsoluteUserPathViolations(text) {
  const violations = [];

  WINDOWS_ABSOLUTE_PATH_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(WINDOWS_ABSOLUTE_PATH_PATTERN)) {
    violations.push({
      rule: "privacy.absolute-user-path",
      line: lineNumberAt(text, match.index),
      message: "absolute Windows path",
    });
  }

  WINDOWS_UNC_PATH_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(WINDOWS_UNC_PATH_PATTERN)) {
    violations.push({
      rule: "privacy.absolute-user-path",
      line: lineNumberAt(text, match.index),
      message: "absolute Windows UNC path",
    });
  }

  POSIX_USER_PATH_PATTERN.lastIndex = 0;
  for (const match of text.matchAll(POSIX_USER_PATH_PATTERN)) {
    const userName = match[1].toLowerCase();
    if (!PLACEHOLDER_USER_NAMES.has(userName)) {
      violations.push({
        rule: "privacy.absolute-user-path",
        line: lineNumberAt(text, match.index),
        message: "absolute POSIX user-home path",
      });
    }
  }

  return violations;
}

function findActionPinViolations(text) {
  const violations = [];
  const lines = text.split(/\r?\n/u);

  for (const [index, line] of lines.entries()) {
    if (/^\s*#/u.test(line)) {
      continue;
    }

    const values = [];
    const blockMatch = line.match(
      /^\s*(?:-\s*)?(?:"uses"|'uses'|uses)\s*:\s*(.*?)\s*$/u,
    );
    if (blockMatch) {
      values.push(blockMatch[1]);
    }

    const flowPattern =
      /(?:^|[,{]\s*)(?:"uses"|'uses'|uses)\s*:\s*([^,}]+)/gu;
    for (const flowMatch of line.matchAll(flowPattern)) {
      values.push(flowMatch[1]);
    }

    for (const rawValue of new Set(values)) {
      let value = rawValue.replace(/\s+#.*$/u, "").trim();
      if (
        (value.startsWith('"') && value.endsWith('"')) ||
        (value.startsWith("'") && value.endsWith("'"))
      ) {
        value = value.slice(1, -1).trim();
      }

      const isLocal = value.startsWith("./");
      const isPinnedDocker =
        /^docker:\/\/[^@\s]+@sha256:[0-9a-f]{64}$/iu.test(value);
      const isPinnedRepository =
        /^[^/@\s]+\/[^@\s]+@[0-9a-f]{40}$/iu.test(value);

      if (!isLocal && !isPinnedDocker && !isPinnedRepository) {
        violations.push({
          rule: "supply-chain.action-not-pinned",
          line: index + 1,
          message: "GitHub Action must use a full 40-character commit SHA",
        });
      }
    }
  }

  return violations;
}

function findWorkflowSecurityViolations(text) {
  const violations = [];
  const lines = text.split(/\r?\n/u);
  let hasExplicitTopLevelPermissions = false;

  for (const [index, line] of lines.entries()) {
    if (/^\s*#/u.test(line)) {
      continue;
    }

    const code = line.replace(/\s+#.*$/u, "");
    if (/^permissions\s*:/u.test(code)) {
      hasExplicitTopLevelPermissions = true;
    }

    const unsafeDefinitions = [
      {
        pattern:
          /^\s{0,4}(?:"pull_request_target"|'pull_request_target'|pull_request_target)\s*:/u,
        rule: "workflow.privileged-pr-trigger",
        message:
          "pull_request_target is not allowed for this public repository",
      },
      {
        pattern: /^\s*permissions\s*:\s*(?:write-all|read-all)\s*$/u,
        rule: "workflow.broad-permissions",
        message:
          "workflow permissions must enumerate only the scopes each job needs",
      },
      {
        pattern: /^\s*secrets\s*:\s*inherit\s*$/u,
        rule: "workflow.inherited-secrets",
        message: "reusable workflows must not inherit every caller secret",
      },
      {
        pattern: /^\s*persist-credentials\s*:\s*true\s*$/u,
        rule: "workflow.persisted-checkout-credentials",
        message: "checkout credentials must not persist after the checkout step",
      },
      {
        pattern: /^\s*runs-on\s*:.*\bself-hosted\b/iu,
        rule: "workflow.self-hosted-runner",
        message:
          "public-repository workflows must not execute untrusted changes on self-hosted runners",
      },
    ];

    for (const definition of unsafeDefinitions) {
      if (definition.pattern.test(code)) {
        violations.push({
          rule: definition.rule,
          line: index + 1,
          message: definition.message,
        });
      }
    }

    const containerMatch = code.match(
      /^\s*(?:container|image)\s*:\s*["']?([^"'#\s]+)["']?\s*$/u,
    );
    if (
      containerMatch &&
      !/@sha256:[0-9a-f]{64}$/iu.test(containerMatch[1])
    ) {
      violations.push({
        rule: "supply-chain.container-not-pinned",
        line: index + 1,
        message: "workflow container images must use an immutable digest",
      });
    }
  }

  if (!hasExplicitTopLevelPermissions) {
    violations.push({
      rule: "workflow.permissions-missing",
      line: 1,
      message:
        "workflow must declare explicit top-level GitHub token permissions",
    });
  }

  for (const [index, line] of lines.entries()) {
    if (!/\buses\s*:\s*actions\/checkout@[0-9a-f]{40}\b/iu.test(line)) {
      continue;
    }

    const usesIndent = line.match(/^\s*/u)[0].length;
    const checkoutBlock = [line];
    for (
      let blockIndex = index + 1;
      blockIndex < lines.length;
      blockIndex += 1
    ) {
      const candidate = lines[blockIndex];
      if (!candidate.trim() || /^\s*#/u.test(candidate)) {
        checkoutBlock.push(candidate);
        continue;
      }

      const candidateIndent = candidate.match(/^\s*/u)[0].length;
      if (
        candidateIndent < usesIndent ||
        (candidateIndent <= usesIndent && /^\s*-\s/u.test(candidate))
      ) {
        break;
      }
      checkoutBlock.push(candidate);
    }

    if (
      !checkoutBlock.some((candidate) =>
        /^\s*persist-credentials\s*:\s*(?:false|"false"|'false')\s*(?:#.*)?$/u.test(
          candidate,
        ),
      )
    ) {
      violations.push({
        rule: "workflow.checkout-credentials-not-disabled",
        line: index + 1,
        message:
          "actions/checkout must set persist-credentials to false explicitly",
      });
    }
  }

  return violations;
}

function isActionConfiguration(relativePath) {
  const lower = relativePath.toLowerCase();
  const isYaml = lower.endsWith(".yml") || lower.endsWith(".yaml");
  return (
    isYaml &&
    (lower.startsWith(".github/workflows/") ||
      lower.startsWith(".github/actions/") ||
      lower === "action.yml" ||
      lower === "action.yaml")
  );
}

function isWorkflowConfiguration(relativePath) {
  const lower = relativePath.toLowerCase();
  return (
    (lower.endsWith(".yml") || lower.endsWith(".yaml")) &&
    lower.startsWith(".github/workflows/")
  );
}

async function inspectLicenseSidecar(filePath) {
  const extension = path.extname(filePath);
  const stem = extension ? filePath.slice(0, -extension.length) : filePath;
  const candidates = [
    `${filePath}.license.json`,
    `${filePath}.LICENSE`,
    `${stem}.license.json`,
    `${stem}.LICENSE`,
    path.join(path.dirname(filePath), "LICENSE"),
  ];

  for (const candidate of candidates) {
    let stats;
    try {
      stats = await lstat(candidate);
    } catch (error) {
      if (error?.code === "ENOENT") {
        continue;
      }
      throw error;
    }

    if (!stats.isFile() || stats.isSymbolicLink()) {
      continue;
    }

    const contents = await readFile(candidate, "utf8");
    if (!contents.trim()) {
      continue;
    }

    if (candidate.toLowerCase().endsWith(".license.json")) {
      let metadata;
      try {
        metadata = JSON.parse(contents);
      } catch {
        continue;
      }

      const license = metadata?.license_spdx;
      const provenance =
        metadata?.source ??
        metadata?.origin ??
        metadata?.generated_by ??
        metadata?.generatedBy ??
        metadata?.generator ??
        metadata?.author;
      if (
        typeof license !== "string" ||
        !APPROVED_PUBLIC_FIXTURE_LICENSES.has(license.trim()) ||
        !isMeaningfulProvenance(provenance)
      ) {
        continue;
      }
    } else {
      const spdxMatch = contents.match(
        /^SPDX-License-Identifier:\s*(\S+)\s*$/imu,
      );
      const sourceMatch = contents.match(/^Source:\s*(.+?)\s*$/imu);
      if (
        !spdxMatch ||
        !APPROVED_PUBLIC_FIXTURE_LICENSES.has(spdxMatch[1]) ||
        !sourceMatch ||
        !isMeaningfulProvenance(sourceMatch[1])
      ) {
        continue;
      }
    }

    return candidate;
  }

  return null;
}

async function trackedExcludedPaths(rootPath) {
  try {
    await lstat(path.join(rootPath, ".git"));
  } catch (error) {
    if (error?.code === "ENOENT") {
      return [];
    }
    throw error;
  }

  const { stdout } = await execFileAsync(
    "git",
    ["-C", rootPath, "ls-files", "-z"],
    {
      encoding: "utf8",
      maxBuffer: 64 * 1024 * 1024,
      windowsHide: true,
    },
  );

  return stdout
    .split("\0")
    .filter(Boolean)
    .map((relativePath) => relativePath.replaceAll("\\", "/"))
    .filter((relativePath) => {
      const rootSegment = relativePath.split("/", 1)[0].toLowerCase();
      return EXCLUDED_DIRECTORIES.has(rootSegment);
    })
    .sort((left, right) => left.localeCompare(right));
}

async function collectEntries(rootPath) {
  const entries = [];

  async function visit(directoryPath, relativeDirectory) {
    const directoryEntries = await readdir(directoryPath, {
      withFileTypes: true,
    });
    directoryEntries.sort((left, right) => left.name.localeCompare(right.name));

    for (const directoryEntry of directoryEntries) {
      const fullPath = path.join(directoryPath, directoryEntry.name);
      const relativePath = normalizeRelative(
        path.join(relativeDirectory, directoryEntry.name),
      );

      if (directoryEntry.isSymbolicLink()) {
        entries.push({
          fullPath,
          relativePath,
          type: "symlink",
        });
        continue;
      }

      if (directoryEntry.isDirectory()) {
        if (!isExcludedRootDirectory(relativePath)) {
          await visit(fullPath, relativePath);
        }
        continue;
      }

      entries.push({
        fullPath,
        relativePath,
        type: directoryEntry.isFile() ? "file" : "special",
      });
    }
  }

  await visit(rootPath, "");
  return entries;
}

export async function scanRepository(
  rootPath,
  { maxFileBytes = DEFAULT_MAX_FILE_BYTES } = {},
) {
  const absoluteRoot = path.resolve(rootPath);
  const rootStats = await lstat(absoluteRoot);
  if (rootStats.isSymbolicLink() || !rootStats.isDirectory()) {
    throw new TypeError("Scan root must be a real directory, not a symlink.");
  }

  const violations = [];
  const entries = await collectEntries(absoluteRoot);
  const excludedButTracked = await trackedExcludedPaths(absoluteRoot);

  for (const relativePath of excludedButTracked) {
    violations.push({
      rule: "repository.tracked-excluded-path",
      path: relativePath,
      message:
        "generated or dependency directories must not contain tracked files",
    });
  }

  for (const entry of entries) {
    if (entry.type === "symlink") {
      violations.push({
        rule: "filesystem.symlink",
        path: entry.relativePath,
        message: "symbolic links are not allowed in the public repository",
      });
      continue;
    }

    if (entry.type !== "file") {
      violations.push({
        rule: "filesystem.special-file",
        path: entry.relativePath,
        message: "only regular files and directories are allowed",
      });
      continue;
    }

    const stats = await lstat(entry.fullPath);
    if (stats.size > maxFileBytes) {
      violations.push({
        rule: "filesystem.large-file",
        path: entry.relativePath,
        message: `file is ${stats.size} bytes; maximum is ${maxFileBytes} bytes`,
      });
      continue;
    }

    const extension = path.extname(entry.relativePath).toLowerCase();
    const forbiddenCategory = extensionCategory(extension);
    if (forbiddenCategory) {
      violations.push({
        rule: "privacy.forbidden-binary",
        path: entry.relativePath,
        message: `${forbiddenCategory} files are not allowed`,
      });
      continue;
    }

    if (MEDIA_EXTENSIONS.has(extension)) {
      if (!isInsidePublicFixtures(entry.relativePath)) {
        violations.push({
          rule: "privacy.media-outside-public-fixtures",
          path: entry.relativePath,
          message: "media is allowed only under tests/fixtures/public",
        });
        continue;
      }

      const sidecar = await inspectLicenseSidecar(entry.fullPath);
      if (!sidecar) {
        violations.push({
          rule: "provenance.fixture-license",
          path: entry.relativePath,
          message:
            "public media fixture requires an adjacent non-empty LICENSE or valid .license.json sidecar",
        });
      }
      continue;
    }

    const buffer = await readFile(entry.fullPath);
    if (buffer.includes(0)) {
      violations.push({
        rule: "privacy.unknown-binary",
        path: entry.relativePath,
        message: "unrecognized binary payload is not allowed",
      });
      continue;
    }

    let text;
    try {
      text = UTF8_DECODER.decode(buffer);
    } catch {
      violations.push({
        rule: "text.invalid-utf8",
        path: entry.relativePath,
        message: "non-media repository files must be valid UTF-8",
      });
      continue;
    }

    const textViolations = [
      ...findSecretViolations(text),
      ...findEmailViolations(text),
      ...findAbsoluteUserPathViolations(text),
      ...(isActionConfiguration(entry.relativePath)
        ? findActionPinViolations(text)
        : []),
      ...(isWorkflowConfiguration(entry.relativePath)
        ? findWorkflowSecurityViolations(text)
        : []),
    ];

    for (const violation of textViolations) {
      violations.push({
        ...violation,
        path: entry.relativePath,
      });
    }
  }

  violations.sort(
    (left, right) =>
      left.path.localeCompare(right.path) ||
      (left.line ?? 0) - (right.line ?? 0) ||
      left.rule.localeCompare(right.rule),
  );

  return {
    root: absoluteRoot,
    filesChecked: entries.filter((entry) => entry.type === "file").length,
    violations,
  };
}

function parseArguments(argumentsList) {
  let root = PROJECT_ROOT;
  let maxFileBytes = DEFAULT_MAX_FILE_BYTES;

  for (let index = 0; index < argumentsList.length; index += 1) {
    const argument = argumentsList[index];
    if (argument === "--root") {
      const value = argumentsList[index + 1];
      if (!value) {
        throw new TypeError("--root requires a directory path.");
      }
      root = value;
      index += 1;
    } else if (argument === "--max-file-bytes") {
      const value = Number(argumentsList[index + 1]);
      if (!Number.isSafeInteger(value) || value <= 0) {
        throw new TypeError("--max-file-bytes requires a positive integer.");
      }
      maxFileBytes = value;
      index += 1;
    } else {
      throw new TypeError(`Unknown argument: ${argument}`);
    }
  }

  return { root, maxFileBytes };
}

async function main() {
  const { root, maxFileBytes } = parseArguments(process.argv.slice(2));
  const result = await scanRepository(root, { maxFileBytes });

  if (result.violations.length > 0) {
    console.error(
      `Public-safety check failed with ${result.violations.length} violation(s):`,
    );
    for (const violation of result.violations) {
      const location = violation.line
        ? `${violation.path}:${violation.line}`
        : violation.path;
      console.error(`- [${violation.rule}] ${location}: ${violation.message}`);
    }
    process.exitCode = 1;
    return;
  }

  console.log(
    `Public-safety check passed (${result.filesChecked} files checked).`,
  );
}

const isMain =
  process.argv[1] &&
  pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url;

if (isMain) {
  main().catch((error) => {
    console.error(`Public-safety check could not run: ${error.message}`);
    process.exitCode = 1;
  });
}
