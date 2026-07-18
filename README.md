# agent-skills

A Claude Code plugin marketplace that packages engineering skills for Axis ACAP application development.

## Overview

This is not an application codebase. It provides **skills** (Markdown) that Claude reads at runtime — there is no build or test step, and the deliverable is the documentation itself.

## Configuration: Environment Variables

Copy `.env.example` to `.env` and edit it.

```bash
DEVICE: Device host or IP address
WEB_USER: Account name for accessing devices via web or vapix
WEB_PASS: Password for accessing devices via web or vapix
```

Set the value for the following variables once the project is set up. These variables are used during development for testing.

```bash
SSHUSER: SSH user name for accessing devices via ssh 
SSHPASS: SSH password for accessing devices via ssh
```

> **Note — SSH host key verification is disabled.** `scripts/run.sh` connects with
> `-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no`, so it never stores or verifies
> the device's host key. This is deliberate: a device's host key changes when it is reflashed or
> reinstalled, and prompting on every change would break automated testing. The trade-off is that
> you lose protection against man-in-the-middle (host impersonation) attacks, so only use these
> scripts against devices on a trusted network.

## Installation

Add this marketplace and install the plugin from within Claude Code:

```
/plugin marketplace add <this repository>
/plugin install acap-skills
```

## Installing in other tools

Each skill is a self-contained `SKILL.md` folder using the standard Agent Skills format, so it works in any tool that discovers skills from a directory. Copy the `skills/building-acap` folder into the location your tool watches.

### (ex.) opencode

opencode discovers skills from several locations, including the Claude-compatible ones. Pick one:

```
# Global (all projects)
cp -r skills/building-acap ~/.config/opencode/skills/
# or reuse the Claude-compatible path
cp -r skills/building-acap ~/.claude/skills/

# Project-scoped
cp -r skills/building-acap <your-project>/.opencode/skills/
```

See the [opencode Skills docs](https://opencode.ai/docs/skills/) for details.
