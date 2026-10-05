---
id: SEC-001
title: Keep secrets out of the repository and output
level: MUST
scope: core
verified-by: [ci]
check: "ci: secret scan (gitleaks) on every pull request"
targets-failure: secret-exposure
observed-on: []
rationale: A committed secret must be treated as leaked even after removal, and agents copy credentials into code, logs, and reports when convenient.
sources: ["practices/security-for-agentic-projects.md"]
---
Read credentials from environment variables or the platform's secret store. Do not write secrets into source, tests, fixtures, logs, commit messages, or your reports, and do not commit `.env` files. If you find a secret already committed, stop and tell the user so they can rotate it.
