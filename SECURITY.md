# Security Policy

## Reporting a vulnerability

Please do not open a public issue or pull request for a security vulnerability in libserum.

Report it by email to **git@kalki.de** instead. Include what you found, the version or commit it applies to,
and the smallest input or steps that reproduce it. A proposed patch is welcome but not required.

You will get a reply as soon as the report has been read. This is a volunteer-maintained project, so there is
no guaranteed response time, but security reports are handled before other work. Please give the fix a
reasonable time to be released before disclosing the problem publicly.

## Scope

In scope is how libserum handles colorization packages, which are downloaded from third parties: `cROMc`,
`cRZ` and `cROM` files, scene CSV files and the authoring sidecars next to them. Bugs in the bundled
third-party code (cereal, lz4, miniz) should go to those projects.

Only the latest release and the default branch are supported; fixes are not backported to older versions.
