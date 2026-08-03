# Command contract

This directory contains the machine-readable contract for the implemented
`nps.command/v1` subset. M1 preserves all M0 envelopes byte-for-byte at the
field-contract level and adds one atomic graph command:

- `adjust.exposure` with `params.ev` in the inclusive range `[-10, 10]`;
- `history.undo` with empty `params`;
- `history.redo` with empty `params`;
- `graph.replace` with a complete strict `nps.edit-graph/v1` object and its
  caller-declared canonical SHA-256.

Every envelope requires `commandId`, `idempotencyKey`, `documentId`,
`expectedRevision`, and an explicit local-only privacy policy:

```json
{
  "network": "deny",
  "cloudInference": "deny"
}
```

Unknown properties are rejected at every object boundary. A future protocol
extension must update the schema, C++ parser, compatibility tests, and protocol
documentation together; implementations must never guess unknown fields or
command kinds.

Before DOM parsing, the transport rejects an empty command, a command larger
than 4 MiB plus 64 KiB of envelope allowance, or more than 64 structural JSON
nesting levels. Parse errors use bounded generic messages and never include
command input, graph contents, local paths, or private values.

## Atomic graph replacement

`graph.replace` carries the complete desired immutable graph:

```json
{
  "kind": "graph.replace",
  "params": {
    "graph": {
      "schema": "nps.edit-graph/v1",
      "graphId": "graph-main",
      "workingColorSpace": "nps.color/scene-linear-rec2020-d65/v1",
      "sourceNodeId": "node-source",
      "outputNodeId": "node-output",
      "nodes": []
    },
    "graphHash": "64-lowercase-hexadecimal-sha256-characters"
  }
}
```

The abbreviated example shows the envelope shape only; an executable graph
must satisfy the edit-graph schema and domain invariants, including a valid
source-to-output topology. The parser rejects unknown or duplicate properties,
parses `params.graph` through the strict edit-graph parser, normalizes it, and
computes SHA-256 over its canonical compact JSON. `params.graphHash` must be
exactly 64 lowercase hexadecimal characters and must equal that computed
digest. Commands constructed directly through C++ are checked against the same
graph and hash invariants by `validate_command`.

The command JSON Schema references the companion
`../edit-graph/nps.edit-graph.v1.schema.json` by that schema's stable `$id`;
schema validators must register both files when validating `graph.replace`.

## Idempotency payload

The idempotency table is keyed by `idempotencyKey`. Its normalized request
payload contains:

- `schema`;
- `documentId`;
- `expectedRevision`;
- `kind`;
- normalized `params`;
- `privacy`.

For `graph.replace`, normalized `params` includes both the complete canonical
graph object and `graphHash`. Node-array presentation order therefore cannot
create a distinct payload after parsing, while any semantic graph change does.

`commandId` is excluded because a transport retry may allocate a new request
ID. `idempotencyKey` is excluded because it is the lookup key. A reused key
with a different normalized payload returns `CMD_IDEMPOTENCY_REUSE`; an exact
retry returns the original result.

## Revision rule

All commands require `expectedRevision`. The project store returns
`REV_CONFLICT` when it does not match the current revision. Successful edits,
undo, and redo all create a new monotonically increasing revision; undo and
redo move the current snapshot pointer rather than decrementing the revision.
