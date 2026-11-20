---
name: Bug report
about: Report a bug with a minimal reproducible example and supporting evidence
title: ''
labels: ''
assignees: ''
---

## Mandatory Requirements — Read Before Submitting

**You must first investigate and attempt to reproduce the issue using your coding agent (e.g., Codex, Claude Code, or a comparable tool).**

**We do not provide free-of-charge bug investigation, debugging, or reproduction services.** You are responsible for investigating and isolating the problem before submitting an issue.

**You must thoroughly search existing open and closed issues before submitting a new report. When reporting a bug, you must follow this issue template and provide a minimal, standalone reproducible example (code and/or USD scene), along with relevant logs and execution evidence. Issues that fail to meet these requirements will be deleted without further investigation. Repeated violations may result in your account being blocked from this repository.**

### Reproduction-First Policy

**Reproduce first, report second.**

You must follow the **[Reproduction and Issue Reporting Guide](../../doc/issue_reporting.md)** before submitting an issue.

The guide covers coding-agent investigation, minimal reproducer creation, verification using GitHub Actions or other free CI services, and collection of execution logs and reproducible evidence.

**Do not claim that a bug has been reproduced without actual execution evidence.** If CI verification is not technically feasible, explain why and provide local reproduction evidence instead.

---

## Pre-Submission Checklist

- [ ] I thoroughly searched existing open and closed issues.
- [ ] I investigated the issue using a coding agent.
- [ ] I created a minimal, standalone reproducible example (code and/or USD scene).
- [ ] I followed the [Reproduction and Issue Reporting Guide](../../doc/issue_reporting.md).
- [ ] I executed the reproducer and verified the actual results.
- [ ] I attempted CI reproduction where technically feasible, or explained why it was not possible.
- [ ] I included reproduction instructions, relevant logs, and execution evidence.
- [ ] I included a CI run URL and reproduction repository URL, if available.
- [ ] I verified that the issue is not caused by incorrect usage, missing dependencies, or configuration errors.

## Describe the Bug

Provide a clear and concise description of the bug.

## To Reproduce

Provide exact steps to reproduce the behavior:

1. ...
2. ...
3. ...
4. Observe the issue.

**Minimal reproduction (repository URL, code, or attached archive):**

**Reproduction commands and dependencies:**

## Reproduction Verification

**Reproduction status:**

- [ ] `REPRODUCED` — The reported behavior was observed and verified.
- [ ] `NOT_REPRODUCED` — The test ran, but the reported behavior was not observed.
- [ ] `INCONCLUSIVE` — Verification was blocked by unrelated failures or environmental limitations.
- [ ] `FLAKY` — The behavior occurs intermittently.

**GitHub Actions / CI run URL:**

**Reproduction repository URL:**

**Execution results and logs:**

**If CI reproduction was not possible, explain why:**

## Expected Behavior

Describe what you expected to happen.

## Actual Behavior

Describe what actually happened, including any errors or unexpected results.

## Reproducible USD Scene, Source Code, and Output Logs

Attach a minimal USD scene (`.usd`, `.usda`, or `.usdc`), relevant source code, and execution logs.

Include all dependencies and instructions necessary to reproduce the issue independently.

## Screenshots

If applicable, attach screenshots showing the expected and actual results.

## Desktop Environment

- OS:
- CPU / Architecture:
- GPU / Driver:
- Browser (if applicable):
- Application / Library Version:
- Git Commit (if applicable):

## Smartphone Environment (if applicable)

- Device:
- OS:
- Browser:
- Version:

## Coding Agent Investigation

**Coding agent used:**

**Investigation summary:**

**Suspected root cause (optional):**

## Additional Context

Add any other relevant information, related issues, or workarounds.
