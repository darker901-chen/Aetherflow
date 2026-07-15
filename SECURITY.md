# Security Policy

AetherFlow processes screen content and applies privacy masks before encoding.
Please treat any issue that could expose unmasked pixels, bypass a mask,
disclose captured data, or weaken local-only execution as security-sensitive.

## Supported version

Security fixes are developed against the latest `master` revision. Older
commits, local forks, and unmaintained packaged builds are not supported.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting flow: open this repository's
**Security** tab and choose **Report a vulnerability**. Include the affected
revision, platform/backend, reproduction steps, impact, and the smallest safe
diagnostic evidence you can provide.

If private vulnerability reporting is not available, contact the repository
owner through the private contact method on their GitHub profile and ask for a
secure reporting channel. Do not put exploit details, captured frames, traces,
credentials, or sensitive screen content in a public issue.

Please allow the maintainer time to reproduce and coordinate a fix before
public disclosure. Ordinary non-sensitive bugs and feature requests can use the
public issue tracker.
