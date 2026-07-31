# agent-skills

> **Disclaimer:** This is an independent project and is not affiliated with, endorsed by, or sponsored by Axis Communications AB.

A Claude Code plugin marketplace packaging engineering skills for Axis ACAP application
development.

This is not an application codebase. Each skill is Markdown that Claude reads at runtime, so
there is no build or test step — the documentation *is* the deliverable.

## Skills

| Skill | What it does | Setup & usage |
|---|---|---|
| [`building-acap`](skills/building-acap/) | Develop, deploy and test ACAP Native SDK applications on Axis devices: project setup, the ACAP APIs (VDO, Larod, Axoverlay 2, Bounding Box, …), and helper scripts for driving a device | [README](skills/building-acap/README.md) |

Skills are distributed **individually** — install only the ones you want. Each has its own README
covering what it needs before use (device credentials, prerequisites, scripts); read that before
installing.

## Installation

Add this marketplace once, then install skills by name:

```
/plugin marketplace add <this repository>
/plugin install building-acap@taki-acap-skills
```

Refresh your local copy of the marketplace after changes are pushed:

```
/plugin marketplace update taki-acap-skills
```

## Installing in other tools

Each skill is a self-contained `SKILL.md` folder in the standard Agent Skills format, so it works
in any tool that discovers skills from a directory. Copy the skill folder into the location your
tool watches.

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

## Repository layout

```
.claude-plugin/marketplace.json   <- one plugin entry per skill
skills/<name>/
├── README.md                     <- setup the skill needs: prerequisites, credentials, scripts
├── SKILL.md                      <- the skill itself; a router into references/
├── references/                   <- per-topic docs, loaded on demand
├── scripts/                      <- helper scripts the skill invokes
└── evals/                        <- test prompts and expectations for the skill
tests/                            <- real projects built with these skills, kept as feedback
```

## Adding a skill

Create `skills/<name>/` with at least a `SKILL.md` and a `README.md`, then add a plugin entry for
it to `.claude-plugin/marketplace.json`:

```json
{
  "name": "<name>",
  "description": "<what it does>",
  "source": "./",
  "strict": false,
  "skills": ["./skills/<name>"]
}
```

The `skills` field matters here: because every entry shares `source: "./"`, the listed paths are
the *complete* set for that entry — a new directory under `skills/` is not picked up until it is
listed. See the
[marketplace docs](https://code.claude.com/docs/en/plugin-marketplaces).

## License

[MIT](LICENSE) © Makoto Takizawa. Each skill declares `license: MIT` in its `SKILL.md`
frontmatter, so the terms travel with the skill when it is copied into another tool.

## Disclaimer

This project is an independent project and is not affiliated with, endorsed by, sponsored by, or otherwise associated with Axis Communications AB.

The views, opinions, and code expressed in this repository are solely those of the author and do not necessarily reflect the views or opinions of Axis Communications AB.

Axis Communications and all related trademarks are the property of their respective owners.
