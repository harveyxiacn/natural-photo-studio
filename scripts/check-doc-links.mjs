#!/usr/bin/env node

import { lstat, readdir, readFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

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

function normalizeRelative(relativePath) {
  return relativePath.split(path.sep).join("/");
}

function insideRoot(root, candidate) {
  const relative = path.relative(root, candidate);
  return (
    relative === "" ||
    (!relative.startsWith(`..${path.sep}`) &&
      relative !== ".." &&
      !path.isAbsolute(relative))
  );
}

async function collectMarkdownFiles(root) {
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
      } else if (entry.isFile() && entry.name.toLowerCase().endsWith(".md")) {
        files.push(fullPath);
      }
    }
  }

  await visit(root);
  return files;
}

function isFence(line) {
  return /^\s{0,3}(?:`{3,}|~{3,})/u.test(line);
}

function decodeEntities(value) {
  return value
    .replaceAll("&amp;", "&")
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&quot;", '"')
    .replaceAll("&#39;", "'");
}

function githubSlug(heading) {
  return decodeEntities(heading)
    .replace(/<[^>]*>/gu, "")
    .replace(/!\[([^\]]*)\]\([^)]*\)/gu, "$1")
    .replace(/\[([^\]]+)\]\([^)]*\)/gu, "$1")
    .replace(/[`*_~]/gu, "")
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{N}\s_-]/gu, "")
    .replace(/\s+/gu, "-");
}

function parseDocument(text) {
  const anchors = new Set();
  const duplicateCounts = new Map();
  const links = [];
  const lines = text.split(/\r?\n/u);
  let inFence = false;

  for (const [index, line] of lines.entries()) {
    if (isFence(line)) {
      inFence = !inFence;
      continue;
    }
    if (inFence) {
      continue;
    }

    const headingMatch = line.match(/^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$/u);
    if (headingMatch) {
      const base = githubSlug(headingMatch[1]);
      if (base) {
        const duplicateCount = duplicateCounts.get(base) ?? 0;
        const slug = duplicateCount === 0 ? base : `${base}-${duplicateCount}`;
        duplicateCounts.set(base, duplicateCount + 1);
        anchors.add(slug);
      }
    }

    for (const match of line.matchAll(
      /<a\s+[^>]*\b(?:id|name)\s*=\s*["']([^"']+)["'][^>]*>/giu,
    )) {
      anchors.add(match[1].toLowerCase());
    }

    for (const match of line.matchAll(/!?\[[^\]]*\]\(([^)]+)\)/gu)) {
      let target = match[1].trim();
      if (target.startsWith("<")) {
        const closing = target.indexOf(">");
        if (closing >= 0) {
          target = target.slice(1, closing);
        }
      } else {
        target = target.split(/\s+(?=["'])/u)[0];
      }
      links.push({ line: index + 1, target });
    }

    const referenceMatch = line.match(
      /^\s{0,3}\[[^\]]+\]:\s*(?:<([^>]+)>|(\S+))/u,
    );
    if (referenceMatch) {
      links.push({
        line: index + 1,
        target: referenceMatch[1] ?? referenceMatch[2],
      });
    }

    for (const match of line.matchAll(
      /<(?:a|img)\s+[^>]*\b(?:href|src)\s*=\s*["']([^"']+)["'][^>]*>/giu,
    )) {
      links.push({ line: index + 1, target: match[1] });
    }
  }

  return { anchors, links };
}

function isExternalTarget(target) {
  return (
    /^[A-Za-z][A-Za-z0-9+.-]*:/u.test(target) ||
    target.startsWith("//")
  );
}

function decodeTarget(target) {
  try {
    return decodeURIComponent(target);
  } catch {
    return null;
  }
}

async function resolveTarget(root, sourceFile, rawTarget) {
  const target = decodeTarget(rawTarget);
  if (target === null) {
    return { error: "link contains invalid percent encoding" };
  }

  const hashIndex = target.indexOf("#");
  const pathAndQuery =
    hashIndex >= 0 ? target.slice(0, hashIndex) : target;
  const anchor = hashIndex >= 0 ? target.slice(hashIndex + 1) : null;
  const targetPath = pathAndQuery.split("?")[0];
  const candidate =
    targetPath === ""
      ? sourceFile
      : targetPath.startsWith("/")
        ? path.resolve(root, `.${targetPath}`)
        : path.resolve(path.dirname(sourceFile), targetPath);

  if (!insideRoot(root, candidate)) {
    return { error: "relative link escapes the repository root" };
  }

  let stats;
  try {
    stats = await lstat(candidate);
  } catch (error) {
    if (error?.code === "ENOENT") {
      return { error: "target does not exist" };
    }
    throw error;
  }

  if (stats.isSymbolicLink()) {
    return { error: "target is a symbolic link" };
  }

  let anchorFile = candidate;
  if (stats.isDirectory() && anchor !== null) {
    anchorFile = path.join(candidate, "README.md");
    try {
      const readmeStats = await lstat(anchorFile);
      if (!readmeStats.isFile()) {
        return { error: "directory anchor has no README.md target" };
      }
    } catch (error) {
      if (error?.code === "ENOENT") {
        return { error: "directory anchor has no README.md target" };
      }
      throw error;
    }
  }

  return { anchor, anchorFile };
}

async function main() {
  const root = PROJECT_ROOT;
  const markdownFiles = await collectMarkdownFiles(root);
  const parsedDocuments = new Map();
  const violations = [];
  let linksChecked = 0;

  for (const markdownFile of markdownFiles) {
    parsedDocuments.set(
      markdownFile,
      parseDocument(await readFile(markdownFile, "utf8")),
    );
  }

  for (const markdownFile of markdownFiles) {
    const document = parsedDocuments.get(markdownFile);
    for (const link of document.links) {
      if (
        !link.target ||
        isExternalTarget(link.target) ||
        link.target.startsWith("mailto:") ||
        link.target.startsWith("tel:")
      ) {
        continue;
      }

      linksChecked += 1;
      const resolved = await resolveTarget(root, markdownFile, link.target);
      if (resolved.error) {
        violations.push({
          file: normalizeRelative(path.relative(root, markdownFile)),
          line: link.line,
          target: link.target,
          message: resolved.error,
        });
        continue;
      }

      if (resolved.anchor !== null && resolved.anchor !== "") {
        const anchorFile = resolved.anchorFile;
        if (path.extname(anchorFile).toLowerCase() !== ".md") {
          continue;
        }
        let targetDocument = parsedDocuments.get(anchorFile);
        if (!targetDocument) {
          targetDocument = parseDocument(await readFile(anchorFile, "utf8"));
          parsedDocuments.set(anchorFile, targetDocument);
        }
        if (!targetDocument.anchors.has(resolved.anchor.toLowerCase())) {
          violations.push({
            file: normalizeRelative(path.relative(root, markdownFile)),
            line: link.line,
            target: link.target,
            message: "target heading anchor does not exist",
          });
        }
      }
    }
  }

  if (violations.length > 0) {
    console.error(
      `Documentation-link check failed with ${violations.length} violation(s):`,
    );
    for (const violation of violations) {
      console.error(
        `- ${violation.file}:${violation.line} -> ${violation.target}: ${violation.message}`,
      );
    }
    process.exitCode = 1;
    return;
  }

  console.log(
    `Documentation-link check passed (${markdownFiles.length} Markdown files, ${linksChecked} internal links).`,
  );
}

main().catch((error) => {
  console.error(`Documentation-link check could not run: ${error.message}`);
  process.exitCode = 1;
});
