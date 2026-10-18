# Signing, trust and distribution

> **Developer preview (RatMode 0.1.9+):** catalogue submissions aren't open yet; for now, build and load your own pipes in Developer mode.

Short and blunt version:

> RatMode only loads bundles signed with the RatMode author's Apple Team ID.
> **You cannot sign your own pipe for distribution.** To ship a pipe, submit its
> *source* to the `ratmode-pipes` catalogue repository; its CI builds, signs and
> notarises it with the author's Developer ID.
> A bundle you build locally loads only with **Developer mode** on, after a consent
> prompt, and is shown with an **Unverified** badge. It is for your own testing and
> is not distributable.

## Why

Third-party pipes run inside the RatMode process with RatMode's own access. In-process
code cannot be fully fenced, so distribution is curated rather than open:

1. A developer submits a pipe as **source** (a pull request to `ratmode-pipes`, not to
   this template repo).
2. Catalogue CI runs, in order: an automated review of the source for malicious intent;
   a header lint that allows only approved headers (no file, network, thread, process
   or dynamic-loading APIs); a **locked build script** that ignores your own build files
   and links only libSystem and libc++; this SDK's validator suite; and a post-link
   **binary permissions check** (an import allowlist: maths, memory and the C++
   runtime; no network, file, exec / spawn or `dlopen` / `dlsym` imports, no extra
   `LC_LOAD_DYLIB`, no raw syscall instructions).
3. The author reviews and merges.
4. CI builds from the merged source, then signs and notarises with the author's
   Developer ID.
5. The author publishes the result for licence holders.

At run time RatMode adds a second line of defence: in normal mode the code signature's
Team ID is checked on **every load** (whatever the quarantine flag says), the same
binary permissions check runs again, and a signed revocation list can stop a specific
pipe version loading at all.

## What this means for you

| | local build (`ratpipe build`) | catalogue build |
|---|---|---|
| Signature | ad-hoc (`codesign --sign -`) so arm64 will run it | author's Developer ID, notarised |
| Loads in normal mode | **no** | yes |
| Loads with Developer mode | yes, after a consent prompt, per binary hash | n/a |
| Badge | Unverified | Verified |
| May be handed to other people | no | yes |

The build system makes your local bundle as close to the catalogue build as it can
(universal slices, macOS 11 minimum, one exported symbol, no rpath, only libSystem and
libc++), so a pipe that validates locally has a good chance of passing the catalogue's
checks. It is not a guarantee: the catalogue build is locked and your build files are
not used there.

## `ratpipe package`

```
tools/ratpipe package MyBass                      # zip with a README that says all of the above
tools/ratpipe package MyBass --identity "Developer ID Application: Your Name (TEAMID)"
tools/ratpipe package MyBass --identity "..." --notarize --keychain-profile MyProfile
```

`package` validates, copies the bundle, optionally re-signs it with `--identity`
(hardened runtime, secure timestamp) and `--notarize` submits the zip with
`xcrun notarytool submit --wait` (store credentials once with
`xcrun notarytool store-credentials`). These options exist for **Developer-mode
testing on other machines** you control, for example a colleague's Mac. **Signing with
your own Developer ID does not make RatMode load the pipe in normal mode**: RatMode
accepts one Team ID and it is not yours. Do not distribute the zip.

## Gatekeeper and quarantine

RatMode does not strip the `com.apple.quarantine` attribute. A zip downloaded in a
browser carries it; a locally built bundle does not. If macOS blocks a quarantined,
ad-hoc-signed bundle, that is Gatekeeper, separate from RatMode's own trust gate;
build locally instead (which is what `ratpipe install` does).

## Entitlements note

RatMode already carries `com.apple.security.cs.disable-library-validation` (it hosts
third-party Audio Units), which is what lets a notarised RatMode load code signed by
a different Team ID at all. Installing pipes in `~/Library` does not affect RatMode's
own notarisation.
