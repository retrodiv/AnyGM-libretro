<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 retrodiv <retrodiv@proton.me> -->

# Security policy

## Supported versions

Security fixes are applied to the latest public release and to the current
development branch when it is preparing the next release. Older releases may
be asked to upgrade when a safe backport is not practical.

## Reporting a vulnerability

Report security issues through the private security-reporting channel offered
by the repository host. If that channel is unavailable, contact a maintainer
privately before disclosing technical details in a public issue.

Include the affected input surface, the smallest synthetic reproducer that can
be shared legally, the observed and expected result, build flags, platform,
and any sanitizer trace. Do not attach private content or credentials.

Maintainers will acknowledge the report, reproduce it against the supported
branch, assess its impact, and coordinate a fix and disclosure. Public details
should wait until users have a reasonable opportunity to update.

## Security boundary

Content, containers, source projects, state buffers, cache files, runtime
override expressions, and paths supplied through the host API are untrusted.
The detailed trust boundaries, resource limits, and validation guarantees are
documented in [the security model](docs/SECURITY_MODEL.md).
