# Packaging and publishing

What is set up in the repository for distributing AsmEditor, and the steps that need the project owner.

## After each release

```powershell
powershell -File tools\update-packaging.ps1 <version>
```

It reads the SHA-256 digests GitHub lists for the release's files and updates the Scoop manifest and the winget
manifests (a folder per version). Commit the result.

## Scoop

`scoop/asmeditor.json` installs the portable zip (settings are kept between updates: `persist: settings`). It works
straight from the repository:

```powershell
scoop install https://raw.githubusercontent.com/Anready/AsmEditor/main/packaging/scoop/asmeditor.json
```

To be found by `scoop search`, the manifest has to be in a bucket: your own (a repository named
`scoop-asmeditor` with the file at the root) or the community `extras` bucket (a pull request to
ScoopInstaller/Extras). `checkver` and `autoupdate` let the bucket's bots follow new releases.

## winget

`winget/manifests/a/Anready/AsmEditor/<version>/` is a winget package for the installer (per-user, silent install
supported). To publish it, open a pull request to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)
with that folder at the same path, or use `wingetcreate submit <folder>`. Check it first:

```powershell
winget validate packaging\winget\manifests\a\Anready\AsmEditor\<version>
winget install --manifest packaging\winget\manifests\a\Anready\AsmEditor\<version>
```

Once accepted: `winget install Anready.AsmEditor`.

## Code signing (SignPath)

Unsigned downloads make SmartScreen warn. The [SignPath Foundation](https://signpath.org/) signs open-source projects
for free. `release.yml` already has the steps; they run when the secret is set:

1. Apply at https://signpath.org/apply (the project has to be open source, with releases built by CI).
2. In SignPath, create the project `AsmEditor` with a signing policy `release-signing` and two artifact
   configurations: `exe` (a zip with `AsmEditor.exe`) and `installer` (a zip with `AsmEditor-setup-*.exe`), each
   signing the PE file inside.
3. Link the GitHub repository as a trusted build system in SignPath.
4. In the repository settings: the secret `SIGNPATH_API_TOKEN` (a CI user's token) and the variable
   `SIGNPATH_ORGANIZATION_ID`.

The next tag then signs `AsmEditor.exe` before it is packed and the installer after it is built. (A certificate of
your own also works: the secrets `SIGNING_CERT`, base64 of a .pfx, and `SIGNING_PASSWORD`.)

## Project page (GitHub Pages)

`docs/index.html` is the project page, with the screenshots from `docs/screenshots`, published by GitHub Pages
(branch `main`, folder `/docs`) at https://anready.github.io/AsmEditor/. Every push to `main` updates it.
