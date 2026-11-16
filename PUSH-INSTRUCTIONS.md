# How to publish v1.1.38

The work is committed locally on `main` and tagged `v1.1.38`. The temporary
GitHub token supplied for the push was rejected by GitHub (`401 Bad
credentials`), so the repository still needs to be pushed from a machine whose
credentials are valid:

```bash
git push origin main
git push origin v1.1.38
```

The tag triggers `.github/workflows/release.yml`, which builds the Windows NSIS
installer and the Linux AppImage and publishes the GitHub Release for
`v1.1.38`.

## Local verification performed before the commit

- `tests/repo_sanity.py`, `tests/plugin_audit.py`, `tests/translation_audit.py`
  and `tests/icon_audit.py` all pass.
- `tests/hotkey_vk_smoke` (83 checks) and `tests/echo_guard_smoke` pass.
- Every touched translation unit was compiled one by one against Qt 6.8
  headers (`-fsyntax-only`) with no warnings from the changed code.
