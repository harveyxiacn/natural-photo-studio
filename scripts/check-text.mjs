#!/usr/bin/env node

import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";
import { TextDecoder } from "node:util";

const EXCLUDED_DIRECTORIES = new Set([
  ".git",
  "node_modules",
  "build",
  ".tools",
  ".tmp",
]);

const PROJECT_ROOT = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "..",
);

const BINARY_EXTENSIONS = new Set([
  ".avif",
  ".bmp",
  ".bz2",
  ".dng",
  ".exe",
  ".gif",
  ".gz",
  ".heic",
  ".ico",
  ".jpeg",
  ".jpg",
  ".mov",
  ".mp3",
  ".mp4",
  ".onnx",
  ".pdf",
  ".png",
  ".psd",
  ".rar",
  ".safetensors",
  ".so",
  ".tif",
  ".tiff",
  ".wav",
  ".webm",
  ".webp",
  ".xz",
  ".zip",
]);

const TEXT_EXTENSIONS = new Set([
  ".c",
  ".cc",
  ".cmake",
  ".cpp",
  ".css",
  ".csv",
  ".cxx",
  ".h",
  ".hh",
  ".hpp",
  ".html",
  ".in",
  ".ini",
  ".js",
  ".json",
  ".jsonl",
  ".mjs",
  ".md",
  ".ps1",
  ".py",
  ".sh",
  ".sql",
  ".svg",
  ".toml",
  ".ts",
  ".tsx",
  ".txt",
  ".xml",
  ".yaml",
  ".yml",
]);

const TEXT_FILE_NAMES = new Set([
  ".clang-format",
  ".clang-tidy",
  ".editorconfig",
  ".gitattributes",
  ".gitignore",
  ".gitmodules",
  ".npmrc",
  "CMakeLists.txt",
  "LICENSE",
  "NOTICE",
]);

const UTF8_DECODER = new TextDecoder("utf-8", { fatal: true });

function hasByteOrderMark(buffer) {
  return (
    (buffer.length >= 3 &&
      buffer[0] === 0xef &&
      buffer[1] === 0xbb &&
      buffer[2] === 0xbf) ||
    (buffer.length >= 2 &&
      ((buffer[0] === 0xff && buffer[1] === 0xfe) ||
        (buffer[0] === 0xfe && buffer[1] === 0xff))) ||
    (buffer.length >= 4 &&
      ((buffer[0] === 0x00 &&
        buffer[1] === 0x00 &&
        buffer[2] === 0xfe &&
        buffer[3] === 0xff) ||
        (buffer[0] === 0xff &&
          buffer[1] === 0xfe &&
          buffer[2] === 0x00 &&
          buffer[3] === 0x00)))
  );
}

function normalizeRelative(relativePath) {
  return relativePath.split(path.sep).join("/");
}

function isExpectedText(relativePath) {
  const name = path.basename(relativePath);
  return (
    TEXT_FILE_NAMES.has(name) ||
    TEXT_EXTENSIONS.has(path.extname(name).toLowerCase())
  );
}

async function collectFiles(root) {
  const files = [];

  async function visit(directory) {
    const entries = await readdir(directory, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name));
    for (const entry of entries) {
      if (
        entry.isDirectory() &&
        EXCLUDED_DIRECTORIES.has(entry.name.toLowerCase())
      ) {
        continue;
      }
      const fullPath = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        await visit(fullPath);
      } else if (entry.isFile()) {
        files.push(fullPath);
      }
    }
  }

  await visit(root);
  return files;
}

async function main() {
  const root = PROJECT_ROOT;
  const files = await collectFiles(root);
  const violations = [];
  let textFilesChecked = 0;

  for (const file of files) {
    const relativePath = normalizeRelative(path.relative(root, file));
    const extension = path.extname(file).toLowerCase();
    if (BINARY_EXTENSIONS.has(extension)) {
      continue;
    }

    const buffer = await readFile(file);
    const expectedText = isExpectedText(relativePath);
    if (!expectedText && buffer.includes(0)) {
      continue;
    }

    if (hasByteOrderMark(buffer)) {
      violations.push({
        rule: "text.bom",
        path: relativePath,
        line: 1,
        message: "byte-order marks are not allowed",
      });
    }

    if (expectedText && buffer.includes(0)) {
      violations.push({
        rule: "text.invalid-utf8",
        path: relativePath,
        line: 1,
        message: "expected UTF-8 text contains NUL bytes",
      });
      continue;
    }

    let text;
    try {
      text = UTF8_DECODER.decode(buffer);
    } catch {
      if (expectedText) {
        violations.push({
          rule: "text.invalid-utf8",
          path: relativePath,
          line: 1,
          message: "expected text file is not valid UTF-8",
        });
      }
      continue;
    }

    textFilesChecked += 1;
    const lines = text.split("\n");
    for (const [index, rawLine] of lines.entries()) {
      const line = rawLine.endsWith("\r") ? rawLine.slice(0, -1) : rawLine;
      if (/[ \t]+$/u.test(line)) {
        violations.push({
          rule: "text.trailing-whitespace",
          path: relativePath,
          line: index + 1,
          message: "trailing spaces or tabs",
        });
      }
      if (/^(?:<{7}|={7}|>{7})(?:\s|$)/u.test(line)) {
        violations.push({
          rule: "text.merge-conflict-marker",
          path: relativePath,
          line: index + 1,
          message: "unresolved merge-conflict marker",
        });
      }
    }
  }

  if (violations.length > 0) {
    console.error(`Text check failed with ${violations.length} violation(s):`);
    for (const violation of violations) {
      console.error(
        `- [${violation.rule}] ${violation.path}:${violation.line}: ${violation.message}`,
      );
    }
    process.exitCode = 1;
    return;
  }

  console.log(
    `Text check passed (${textFilesChecked} UTF-8 text files checked).`,
  );
}

main().catch((error) => {
  console.error(`Text check could not run: ${error.message}`);
  process.exitCode = 1;
});
