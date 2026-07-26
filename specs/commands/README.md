# M0 command contract

This directory contains the machine-readable contract for the first vertical
milestone. It deliberately implements a strict subset of `nps.command/v1`:

- `adjust.exposure` with `params.ev` in the inclusive range `[-10, 10]`;
- `history.undo` with empty `params`;
- `history.redo` with empty `params`.

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

## Idempotency payload

The idempotency table is keyed by `idempotencyKey`. Its normalized request
payload contains:

- `schema`;
- `documentId`;
- `expectedRevision`;
- `kind`;
- normalized `params`;
- `privacy`.

`commandId` is excluded because a transport retry may allocate a new request
ID. `idempotencyKey` is excluded because it is the lookup key. A reused key
with a different normalized payload returns `CMD_IDEMPOTENCY_REUSE`; an exact
retry returns the original result.

## Revision rule

All three commands require `expectedRevision`. The project store returns
`REV_CONFLICT` when it does not match the current revision. Successful edits,
undo, and redo all create a new monotonically increasing revision; undo and
redo move the current snapshot pointer rather than decrementing the revision.
