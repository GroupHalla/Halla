# Community add-on catalog

> **The official catalog has moved.** Since Halla Desktop 1.1.11 the
> application reads the official add-on hub at
> **https://grouphalla.github.io/Halla-Addons/** (repository
> [GroupHalla/Halla-Addons](https://github.com/GroupHalla/Halla-Addons)),
> with platform tags (Desktop/Mobile), versions and updates.
> This file remains only for old clients and as an example of the
> v1 format — `catalog.json` is still valid, but empty.

The **Add-ons** tab of old Halla Desktop versions reads `catalog.json`
directly from this directory. To suggest a community package:

1. publish the `.halla-addon` at a stable HTTPS URL;
2. also publish its SHA-256;
3. submit a contribution adding the entry to the `addons` array;
4. provide the source code, author, license and instructions to reproduce
   the DLL.

Inclusion in the catalog does not turn a community DLL into official code.
Halla always shows the native execution warning and validates the SHA-256
before installation.

The complete format is documented in [`docs/PLUGINS.md`](../docs/PLUGINS.md).
