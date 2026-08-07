# building-acap

Develop, deploy and test ACAP (Axis Camera Application Platform) applications with the ACAP
Native SDK.

The skill covers project setup (manifest, Dockerfile, Makefiles), the ACAP APIs — VDO, Larod,
Axoverlay 2, Bounding Box, AXParameter, Edge Storage, Event, Serial, Device Data Hub, License Key
— and a set of scripts for driving a real device. The skill itself is [SKILL.md](SKILL.md), which
routes into [`references/`](references/) per topic.

## Installation

```
/plugin marketplace add <this repository>
/plugin install building-acap@taki-acap-skills
```

For other tools, see the [top-level README](../../README.md#installing-in-other-tools).

## Prerequisites

- An **Axis device** reachable over the network, with **Developer Mode** enabled (this is what
  creates the app's dedicated SSH user on install).
- **Docker** — the SDK is not installed on the host; the app is cross-compiled in a container.
- **`curl`**, **`ssh`** and **`sshpass`** on the host, used by the scripts below.

## Configuration

The scripts read device credentials from a `.env` file in **your ACAP project root** (not in this
skill directory). Copy [`.env.example`](.env.example) there and fill it in:

```bash
cp .env.example <your-acap-project>/.env
```

| Variable | Meaning |
|---|---|
| `DEVICE` | Device host name or IP address |
| `WEB_USER` | Account name for accessing the device via web / VAPIX |
| `WEB_PASS` | Password for the above |
| `SSH_PASS` | SSH password used during development; set it on the device with `setup_ssh.sh` |

The application name is deliberately *not* in here — it is passed to each script as an argument
instead, so one `.env` serves every project on the device. Installing an app creates its dedicated
SSH user as `acap-<appName>`, and the scripts derive that name from the argument you give them.

`.env` holds real credentials, so it is gitignored — keep it out of version control, and note
that the skill instructs Claude never to read or write it.

> **Note — SSH host key verification is disabled.** `scripts/run.sh` connects with
> `-o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no`, so it never stores or verifies
> the device's host key. This is deliberate: a device's host key changes when it is reflashed or
> reinstalled, and prompting on every change would break automated testing. The trade-off is that
> you lose protection against man-in-the-middle (host impersonation) attacks, so only use these
> scripts against devices on a trusted network.

## Scripts

Run them **with `bash`, from your project root** — they are not all marked executable, and each
sources `./.env` by relative path. A `.env missing` message usually means the working directory
drifted rather than anything being wrong with the device.

| Script | Purpose |
|---|---|
| `bash scripts/deploy.sh <file>.eap` | Upload and install the package on the device |
| `bash scripts/setup_ssh.sh <appName>` | Set the password of the app's `acap-<appName>` SSH user. Run once after the first install |
| `bash scripts/control.sh <appName> start\|stop\|restart\|remove` | Control the installed app |
| `bash scripts/run.sh <appName> <binary> [-a "args"]` | Run a binary from the installed package over SSH; stdout is captured to the `output` file |
| `bash scripts/view_log.sh <binary-name>` | Show the syslog output for an app or test binary |

Two traps worth knowing before you start, because neither announces itself:

- **`deploy.sh` resets the app's log.** Always go deploy → run → read log; a deploy slipped
  between running something and reading its log discards exactly the output you wanted.
- **`control.sh remove` deletes the app's SSH user and its password**, so you have to set it
  again before `run.sh` works. Between iterations, install over the top instead of removing.

## Usage

Once installed, the skill triggers on ACAP work automatically. It builds in two phases and will
not skip the first: a **walking skeleton** (a Hello World app driven all the way through build →
package → install → start → log → SSH) to prove the environment, and only then the feature
itself. That ordering exists because the toolchain has many environment-specific failure points,
and debugging your code and your environment simultaneously is what costs the day.

See [SKILL.md](SKILL.md) for the full workflow and the per-API manifest requirements.
