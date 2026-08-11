# Release security

Official tagged Windows releases require Authenticode signing.

Repository secrets:

- `WINDOWS_SIGNING_PFX_BASE64`
- `WINDOWS_SIGNING_PFX_PASSWORD`

Optional repository variable: `WINDOWS_TIMESTAMP_URL`.

The updater accepts only the exact installer name for the semantic version,
requires its `.sha256`, limits downloads to 200 MiB and validates both SHA-256
and Authenticode before execution. Private identities and bookmark passwords
are stored with QtKeychain in the operating-system credential vault.
