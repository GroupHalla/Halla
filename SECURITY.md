# Release security

Windows releases support optional Authenticode signing through:

- `WINDOWS_SIGNING_PFX_BASE64`
- `WINDOWS_SIGNING_PFX_PASSWORD`
- optional variable `WINDOWS_TIMESTAMP_URL`

Until a trusted certificate is configured, tagged releases remain unsigned.
The updater still requires the exact installer name, HTTPS, a matching
`.sha256` asset and a maximum size of 200 MiB. For an unsigned installer it
shows a prominent “Editor desconhecido” warning and requires an additional,
non-default user confirmation. Once a certificate is configured, Authenticode
is validated automatically.

Private identities and bookmark passwords are stored with QtKeychain in the
operating-system credential vault.
