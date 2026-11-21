# Repro-First GitHub Issue Reporting Guide

## 1. Purpose

This document defines the recommended workflow for investigating, reproducing, and reporting bugs to GitHub repositories.

**Core principle: Reproduce first, report second.**

Before submitting a GitHub Issue, make a reasonable effort to reproduce the problem in an isolated, reproducible environment, preferably using a publicly accessible CI service such as GitHub Actions.

The goal is to provide maintainers with actionable bug reports backed by executable reproduction cases, logs, environment information, and CI run URLs.

This workflow is particularly useful for AI coding agents that autonomously investigate bugs and prepare upstream issue reports.

## 2. Reporting Policy for Coding Agents

When preparing a GitHub Issue, follow these rules:

1. **Search existing issues first.** Avoid duplicate reports.
2. **Identify the affected versions and environment.** Record the exact repository commit, dependency versions, platform, and relevant configuration.
3. **Create a minimal reproducer.** Isolate the problematic behavior and eliminate unrelated dependencies.
4. **Run the reproducer locally when possible.**
5. **Verify the reproducer using CI.** Prefer GitHub Actions or another independently accessible execution environment.
6. **Collect reproducible evidence.** Preserve logs, exit codes, stack traces, and the expected versus actual behavior.
7. **Classify the reproduction result.** Distinguish confirmed bugs from unsuccessful or inconclusive attempts.
8. **Prepare an actionable GitHub Issue.** Include reproduction instructions and links to evidence.
9. **Do not claim reproduction success without execution evidence.**
10. **Submit only when authorized.** If issue submission has not been authorized, prepare a draft instead.

Never invent reproduction logs, CI results, execution URLs, or successful test outcomes.

### Mandatory Evidence Gate

Before reporting a bug as confirmed, verify all of the following:

- The reproducer actually ran.
- The observed failure matches the suspected bug.
- The failure is not merely an unrelated dependency, installation, or infrastructure error.
- The environment and dependency versions are documented.
- The reproduction can be repeated by the maintainer.

A failed CI job is not automatically evidence of a reproduced bug. The failure must correspond to the behavior being reported.

If a problem cannot be reproduced, it may still be reported when appropriate, but its unconfirmed status must be disclosed.

## 3. Recommended CI and Reproduction Services

### 3.1 CI Services

| Service | Best use cases | Free usage considerations |
|---|---|---|
| [GitHub Actions](https://github.com/features/actions) | General-purpose reproduction, Linux/Windows/macOS, C/C++, Python, Rust | Standard GitHub-hosted runners are free for public repositories |
| [CircleCI](https://circleci.com/) | Docker-based environments, parallel tests | Free plan with credit-based limits |
| [GitLab CI/CD](https://docs.gitlab.com/ci/) | Independent CI verification, container pipelines | Limited free compute minutes |
| [Buildkite](https://buildkite.com/) | Parallel and customizable pipelines | Free-tier limits and agent requirements vary |
| [Azure Pipelines](https://azure.microsoft.com/en-us/products/devops/pipelines) | Cross-platform builds, Windows testing | Free hosted capacity subject to eligibility and allocation |
| [CodeSandbox](https://codesandbox.io/) | JavaScript, Node.js, browser application reproducers | Plan-dependent free usage |
| [StackBlitz](https://stackblitz.com/) | Frontend and browser reproduction cases | Free public-project capabilities subject to plan restrictions |

Check current pricing and quotas before selecting a provider. Free plans, runner availability, and resource limits may change.

### Recommended Default

**Use GitHub Actions with a dedicated public reproduction repository whenever possible.**

Advantages:

- Direct integration with GitHub
- Public execution logs and run URLs
- Reproducible workflow definitions
- Cross-platform runner matrices
- Artifact uploads
- GitHub CLI and REST API support
- Minimal external infrastructure

For reporting issues to third-party OSS projects, use a reproduction repository under your own control instead of expecting the upstream maintainers to execute untrusted code.

### 3.2 Specialized Environments

For different categories of bugs, consider:

- **Browser/UI:** StackBlitz, Playwright, GitHub Actions
- **C/C++:** GitHub Actions, AddressSanitizer, UndefinedBehaviorSanitizer
- **Python:** pytest, Docker, GitHub Actions
- **Rust:** cargo test, GitHub Actions
- **Compiler bugs:** Minimal source files, compiler version matrices
- **GPU/CUDA/ROCm:** Dedicated GPU runners, appropriately isolated self-hosted infrastructure, or GPU-enabled sandbox providers
- **Platform-specific failures:** Windows/macOS/Linux CI matrices

Standard GitHub-hosted runners do not provide a general-purpose GPU testing environment. GPU-dependent reproduction requires suitable hardware and drivers.

## 4. Existing Bug Reproduction Tools

The following projects may help automate parts of the reproduction workflow.

They should be evaluated before production use. Their inclusion here is not an endorsement of their security, reliability, or maintenance status.

### 4.1 Ghost Hunter

Repository: https://github.com/qxmcu/ghost-hunter

Ghost Hunter is an automated GitHub issue reproduction bot.

Its documented workflow includes:

- Triggering reproduction through a GitHub issue comment such as `bot/reproduce`
- Interpreting reproduction instructions using an LLM
- Executing the reproduction inside an isolated Docker environment
- Collecting execution logs
- Posting reproduction results back to the issue

**Best suited for:** Automatically attempting to reproduce existing GitHub Issues.

It can serve as a reference implementation for automated issue investigation.

### 4.2 issue2repro

Repository: https://github.com/andreruizloera/issue2repro

A project designed to turn GitHub bug reports into reproducible local environments.

**Best suited for:** Generating reproduction environments from issue descriptions.

Its approach is useful when designing an automated pipeline that transforms unstructured bug descriptions into executable test cases.

### 4.3 ReproAgent

Repository: https://github.com/kyy-logs/ReproAgent

An evidence-oriented Python bug reproduction agent with pytest verification and replay-oriented capabilities.

**Best suited for:** Python issue reproduction and verification workflows.

### 4.4 ReproGate

Repository: https://github.com/JinRudy/reprogate

A GitHub Action and CLI tool that checks whether issues or pull requests contain essential reproduction information, such as steps, environment details, and logs.

**Best suited for:** Quality gates for bug reports.

ReproGate checks the completeness of reproduction evidence; it does not replace actual execution of the reproducer.

### 4.5 FailTrace

Repository: https://github.com/Ri4dH/failtrace

A tool oriented toward reproducing CI failures locally.

**Best suited for:** Investigating failures that have already occurred in CI.

### Recommended Combination

For autonomous issue reporting, consider combining:

1. A coding agent to investigate the bug and construct the reproducer
2. A dedicated GitHub repository to publish reproducible test cases
3. GitHub Actions to execute and preserve evidence
4. ReproGate or a custom validator to check report completeness
5. GitHub CLI to prepare and submit the final Issue

Ghost Hunter, issue2repro, and ReproAgent can provide useful implementation ideas for automated reproduction.

## 5. Reference Architecture

The recommended workflow is:

```text
Bug discovery
      |
      v
Search existing GitHub Issues
      |
      v
Coding Agent investigates
      |
      v
Generate minimal reproducer
      |
      v
Run local tests
      |
      v
Create/update public Repro Repository
      |
      v
Trigger GitHub Actions
      |
      v
Inspect CI logs and artifacts
      |
      v
Evidence Validation Gate
      |
      +---- REPRODUCED
      |
      +---- NOT_REPRODUCED
      |
      +---- INCONCLUSIVE
      |
      +---- FLAKY
      |
      v
Prepare GitHub Issue
      |
      v
Submit when authorized
```

### Reproduction Status Definitions

| Status | Meaning |
|---|---|
| `REPRODUCED` | The expected bug behavior was observed and verified |
| `NOT_REPRODUCED` | The reproduction executed successfully, but the suspected bug was not observed |
| `INCONCLUSIVE` | Testing could not establish a result because of unrelated failures or limitations |
| `FLAKY` | The bug is reproducible intermittently, with an observed reproduction rate |

For flaky failures, record:

- Number of attempts
- Number of matching failures
- Relevant timing or concurrency parameters
- Environment differences
- Random seeds, where applicable

A stronger demonstration is a **regression comparison**, where the same reproduction test fails against a buggy version and passes against a known fixed version.

## 6. Dedicated Reproduction Repository

Use a dedicated public GitHub repository for reproduction cases when reporting bugs to external projects.

Suggested layout:

```text
issue-repros/
├── README.md
├── issue_reporting.md
├── repros/
│   └── example-bug/
│       ├── README.md
│       ├── repro.py
│       ├── requirements.txt
│       └── evidence/
└── .github/
    └── workflows/
        └── repro.yml
```

Each reproduction case should contain:

- A concise description of the problem
- Upstream repository URL
- Affected commit or release
- Minimal source code
- Dependency specifications
- Environment requirements
- Expected behavior
- Actual behavior
- Execution instructions
- Relevant logs and CI URLs

Prefer pinned dependency versions and immutable upstream commit hashes.

For complex dependencies, use a Dockerfile or a versioned container image. Record the image digest where practical.

The reproduction should be independently executable without relying on undocumented local machine state.

## 7. Example GitHub Actions Workflow

Create `.github/workflows/repro.yml`:

```yaml
name: Bug Reproduction

on:
  workflow_dispatch:
  push:
    paths:
      - "repros/**"
      - ".github/workflows/repro.yml"

permissions:
  contents: read

jobs:
  reproduce:
    strategy:
      fail-fast: false
      matrix:
        os:
          - ubuntu-latest
          - windows-latest
          - macos-latest

    runs-on: ${{ matrix.os }}
    timeout-minutes: 15

    defaults:
      run:
        working-directory: repros/example-bug

    steps:
      - name: Checkout repository
        uses: actions/checkout@v4

      - name: Set up Python
        uses: actions/setup-python@v5
        with:
          python-version: "3.12"

      - name: Install dependencies
        run: python -m pip install -r requirements.txt

      - name: Run reproduction
        run: python repro.py

      - name: Upload evidence
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: repro-${{ matrix.os }}
          path: repros/example-bug/evidence/
          if-no-files-found: ignore
```

This example assumes `repro.py` exits with a nonzero code when the expected bug is detected.

A red CI result is acceptable for an intentionally failing reproducer, provided the logs demonstrate the correct failure.

For an automated evidence gate, a better approach is to distinguish the reproduction outcome from the CI job status. For example, the test harness can emit a structured JSON result, confirm the expected error signature, and exit successfully when the bug is reproduced as expected.

### Suggested Structured Result

```json
{
  "status": "REPRODUCED",
  "upstream_repository": "owner/project",
  "upstream_commit": "<full-commit-sha>",
  "platform": "ubuntu-latest",
  "expected": "Operation returns a valid result",
  "observed": "Operation raises the target exception",
  "attempts": 3,
  "matching_failures": 3,
  "evidence": [
    "repro.log"
  ]
}
```

Only emit `REPRODUCED` after inspecting the actual execution result and confirming the bug-specific failure condition.

## 8. Running CI from a Coding Agent

GitHub CLI documentation: https://cli.github.com/manual/

Assuming GitHub CLI authentication and appropriate repository permissions:

```bash
# Trigger the reproduction workflow
gh workflow run repro.yml \
  --repo OWNER/REPRO_REPO \
  --ref main

# List recent workflow runs
gh run list \
  --repo OWNER/REPRO_REPO \
  --workflow repro.yml

# Inspect a particular run
gh run view RUN_ID \
  --repo OWNER/REPRO_REPO

# Inspect its logs
gh run view RUN_ID \
  --repo OWNER/REPRO_REPO \
  --log

# Download the artifacts
gh run download RUN_ID \
  --repo OWNER/REPRO_REPO \
  --dir ./artifacts
```

Use the specific run ID, commit SHA, and workflow URL when preparing the report.

Do not confuse successful workflow dispatch with completed reproduction. Wait for the run to finish and inspect its output.

## 9. Evidence Validation Gate

Before preparing the final Issue, evaluate the collected evidence.

### Required Checks

- [ ] Reproducer source code is available.
- [ ] Reproduction steps are documented.
- [ ] Target repository and affected version are identified.
- [ ] Relevant dependencies are pinned.
- [ ] The test was actually executed.
- [ ] The execution result has been inspected.
- [ ] The observed result matches the reported bug.
- [ ] The result is classified correctly.
- [ ] Logs and error messages are preserved.
- [ ] CI run URLs or equivalent execution evidence are included.
- [ ] The reproducer does not contain credentials or sensitive information.
- [ ] Existing upstream issues have been checked for duplicates.
- [ ] Issue submission is authorized.

If any mandatory verification fails, do not classify the issue as `REPRODUCED`.

Instead, continue investigation or explicitly report the evidence as incomplete.

### Optional Stronger Verification

When feasible:

1. Execute the same test against the known buggy version.
2. Execute it against a newer or known fixed version.
3. Verify that the relevant failure disappears.
4. Repeat across platforms or dependency versions.
5. Minimize the reproducer while preserving the bug.

This helps distinguish genuine regressions from configuration problems or unrelated failures.

## 10. GitHub Issue Template

Use the following structure for the final report.

```markdown
## Summary

A concise description of the bug.

## Affected Version

- Repository:
- Commit/release:
- OS:
- Runtime/compiler:
- Dependencies:

## Expected Behavior

Describe the intended behavior.

## Actual Behavior

Describe the observed incorrect behavior.

## Minimal Reproduction

Repository:
https://github.com/OWNER/REPRO_REPO

Reproduction file:
https://github.com/OWNER/REPRO_REPO/blob/COMMIT/repros/BUG/repro.py

Steps:

1. Clone the reproduction repository.
2. Install the documented dependencies.
3. Execute the reproducer.
4. Observe the failure.

## CI Verification

Status: REPRODUCED / NOT_REPRODUCED / INCONCLUSIVE / FLAKY

GitHub Actions run:
https://github.com/OWNER/REPRO_REPO/actions/runs/RUN_ID

Platforms tested:

- Ubuntu:
- Windows:
- macOS:

## Execution Evidence

Relevant error messages, stack traces, and
actual output.

## Additional Notes

Relevant configuration details, regression
comparisons, and possible causes, clearly
distinguishing hypotheses from verified facts.
```

When possible, point to an immutable commit rather than a moving branch.

Avoid overwhelming maintainers with unfiltered logs. Include a concise excerpt in the Issue and link to complete evidence.

## 11. Security and Resource Usage

Automated reproduction frequently involves executing code from untrusted sources.

Follow these precautions:

- Use isolated, disposable execution environments.
- Do not expose API keys, personal tokens, or production credentials.
- Use minimal GitHub workflow permissions.
- Avoid executing untrusted code in privileged workflows.
- Do not run untrusted pull-request code through `pull_request_target` with elevated privileges or repository secrets.
- Avoid mounting host filesystems or Docker sockets into untrusted containers.
- Restrict network access when practical.
- Set CPU, memory, storage, and time limits.
- Avoid publishing sensitive logs or private source code.
- Never assume a Docker container alone provides a complete security boundary.
- Use dedicated sandbox infrastructure for high-risk code.

**Free CI must not be treated as unlimited compute infrastructure.**

Respect provider terms of service, repository policies, resource quotas, and abuse-prevention limits.

Do not use automated issue reporting to spam upstream projects.

For public repositories, remember that source files, workflow logs, and uploaded artifacts may be accessible to third parties.

GitHub Actions artifacts and logs can expire. Retain important evidence in a safe, reproducible form rather than relying indefinitely on artifact URLs.

## 12. Automation Improvements

For more advanced coding-agent workflows, consider adding:

### Automatic Reproducer Minimization

Iteratively remove unnecessary code while verifying that the failure still occurs.

### Environment Matrix Search

Find the smallest set of conditions that produces the bug:

- Operating system
- Compiler version
- Library version
- Runtime version
- Dependency combinations
- Hardware architecture

### Regression Bisecting

Use `git bisect` or an equivalent automated search to identify the first failing commit.

### Machine-Readable Evidence

Store reproduction results as JSON to enable deterministic verification and downstream automation.

### Reproduction Confidence

Evaluate confidence using:

- Whether the observed failure matches the target symptom
- Whether multiple executions agree
- Whether unrelated infrastructure errors were excluded
- Whether the environment is sufficiently pinned
- Whether a fixed-version comparison is available

Do not substitute a model-generated confidence score for actual execution evidence.

### Report Review

An agent may autonomously investigate and prepare an Issue, but submission should comply with the user's authorization and the target repository's contribution guidelines.

## 13. Recommended Default Workflow

For most public OSS bug reports:

1. Search existing issues.
2. Create a minimal reproducer.
3. Run the reproducer locally.
4. Publish it in a dedicated public GitHub repository.
5. Execute it through GitHub Actions.
6. Validate the observed failure and record the reproduction status.
7. Preserve logs, artifacts, and immutable source references.
8. Draft the GitHub Issue using the template above.
9. Submit the Issue when authorized.

For coding agents, the key invariant is:

**Never report a bug as successfully reproduced unless the relevant behavior has actually been observed and verified.**

When reproduction is impossible or inconclusive, document the limitation instead of fabricating certainty.

## 14. References

### CI and Reproduction Services

- [GitHub Actions](https://docs.github.com/en/actions)
- [GitHub Actions Billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions)
- [CircleCI](https://circleci.com/)
- [GitLab CI/CD](https://docs.gitlab.com/ci/)
- [Buildkite](https://buildkite.com/)
- [Azure Pipelines](https://azure.microsoft.com/en-us/products/devops/pipelines)
- [CodeSandbox](https://codesandbox.io/)
- [StackBlitz](https://stackblitz.com/)

### Automated Reproduction Projects

- [Ghost Hunter](https://github.com/qxmcu/ghost-hunter)
- [issue2repro](https://github.com/andreruizloera/issue2repro)
- [ReproAgent](https://github.com/kyy-logs/ReproAgent)
- [ReproGate](https://github.com/JinRudy/reprogate)
- [FailTrace](https://github.com/Ri4dH/failtrace)

### GitHub CLI and Documentation

- [GitHub CLI](https://cli.github.com/manual/)
- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [GitHub Issue Creation](https://docs.github.com/en/issues/tracking-your-work-with-issues/using-issues/creating-an-issue)

---

*This document describes a recommended reproduction-first workflow. Individual tools and services may change over time; verify their current capabilities and usage terms before adoption.*
