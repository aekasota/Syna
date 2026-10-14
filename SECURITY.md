# Security policy

## Supported versions

Security fixes go into the latest release. Earlier versions are not patched; upgrading is the fix.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: on the repository's **Security** tab, choose
**Report a vulnerability**. Please do not open a public issue for them.

Useful details:

- the affected version and Windows version;
- what an attacker can do and under which conditions (for example, a crafted video file opened in
  Syna, or a modified installer);
- steps or a file that reproduce it.

Syna is maintained by one person. Reports are acknowledged as soon as possible, and a fix is
prioritized by severity. You will be credited in the release notes unless you prefer not to be.

## Scope

Syna opens untrusted media files, so problems in how they are parsed and decoded matter most. Syna
decodes through FFmpeg: if the flaw is in FFmpeg itself, it is also worth reporting to the FFmpeg
project (<https://ffmpeg.org/security.html>), and Syna will move to a fixed FFmpeg build.

Releases are not code-signed yet, so the authenticity of a download rests on GitHub and on the
published SHA-256 ([docs/PACKAGING.md](docs/PACKAGING.md#signing)).
