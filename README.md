# agent-skills

A Claude Code plugin marketplace that packages engineering skills for Axis ACAP application development.

## Overview

This is not an application codebase. It provides **skills** (Markdown) that Claude reads at runtime — there is no build or test step, and the deliverable is the documentation itself.

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
