# Design — Portfolio-grade repository (security-engineering showcase)

**Date:** 2026-08-20
**Goal:** Turn the NMS project into a repository that wins security / information-engineering
job interviews when browsed cold on GitHub and shared from LinkedIn.
**Scope chosen:** Presentation polish + a security showcase document + one end-to-end
attack-replay test ("B + attack-replay test").

---

## 1. Positioning

One coherent, security-first story:

> A distributed wireless-security monitoring system — ESP32 probes and a Python
> command-and-control server speaking a formally-specified MQTT protocol, with a live
> web console and a wireless intrusion-detection system, built for isolated-lab
> security research.

The firmware and distributed-systems depth reinforce a "computer engineer who works
top-to-bottom" impression; the wireless IDS, threat model, and responsible dual-use
handling are the lead differentiators for a security audience.

**Audience:** cold GitHub visitors and LinkedIn click-throughs. Optimise the first 20
seconds (hero + demo) and the "is this person rigorous?" scan (tests, CI, threat model).

**Voice:** first-person / neutral project voice, as the sole author. No references to
any development tooling anywhere in tracked, portfolio-facing files.

---

## 2. Deliverables

### 2.1 README.md (the hero)

Currently empty (0 bytes). Rebuild top-to-bottom:

1. Title + one-line tagline.
2. Badge row: CI (passing) · license (MIT) · Python version · test count.
3. **Demo GIF of the live console**, immediately under the title.
4. Two-sentence "What it is".
5. **Architecture diagram** — a Mermaid diagram (renders natively on GitHub) of the
   topology: ESP32 probes ⇄ Mosquitto ⇄ Flask C2 server ⇄ browser (SSE).
6. Feature highlights in **security-first** order: wireless IDS (deauth flood / rogue AP
   / evil twin) → multi-vantage RF survey → formally-specified protocol with golden-fixture
   conformance → live SSE operations console.
7. Dashboard screenshots (Fleet / Jobs / RF Survey / Security tabs).
8. Quickstart: `python scripts/run_all.py` (broker + server + virtual probe in one command).
9. Tech stack.
10. **Security & Ethics** callout linking to `docs/SECURITY.md`.
11. Project structure (the four subsystems).
12. Testing (how to run; what the suite covers).
13. License & acknowledgements (MIT; third-party attribution per `NOTICE`).

### 2.2 docs/SECURITY.md (the differentiator)

The document a security hiring manager reads before deciding to interview:

- **Protocol threat model:** `node_id` spoofing bounded by the `^probe-…$` grammar and
  broker ACLs; every inbound payload schema-validated at the boundary before touching the
  database; QoS-1 redelivery made safe by idempotent `(job_id, seq)` chunk storage; the
  1024-byte payload cap; broker credential/ACL generation model.
- **The three IDS detectors:** how each works, with a sample alert JSON payload and how it
  surfaces in the console timeline.
- **Responsible dual-use statement:** the 802.11 deauthentication frame is defined for
  detection and IDS testing; the transmit path is deliberately left unwired. All testing
  is on isolated lab networks owned by the author. References: Espressif ESP-IDF API docs
  and the MIT-licensed ESP32-Bit-Pirate reference (attributed in `NOTICE`).
- **Detection-first philosophy**, stated plainly.

### 2.3 Attack-replay test (`tests/test_attack_replay.py`)

One readable end-to-end test proving the detect → store → API pipeline, with no broker:

- Feed a `wifi_ids` result chunk whose payload carries an `alerts` array containing a
  `deauth_flood` alert (`source_mac`, `target_mac`, `count`, `channel`) through the ingest
  chunk handler that calls `_project_ids_alerts` (`server/ingest.py:182`).
- Assert an `IdsAlert` row is created with `alert_type == "deauth_flood"` and the correct
  `source_mac` / `count` / `channel`.
- Assert `GET /api/security/alerts` returns the alert.
- Follow the existing ingest-test fixtures/pattern (handlers take an already-validated dict;
  no MQTT, no threading).

### 2.4 CI (`.github/workflows/ci.yml`)

On push and pull request: check out, set up Python, install `requirements-dev.txt`, run
`pytest`. The conformance suite starts a throwaway Mosquitto container via Docker, which is
available on GitHub's `ubuntu-latest` runners, so those scenarios run in CI rather than
skipping. Implementation must confirm the suite stays green and the run is what backs the
"passing" badge (no green-by-skip). Required env vars for the app factory
(`SECRET_KEY`, `NMS_ADMIN_PASSWORD_HASH`, etc.) are supplied as workflow env / test config.

### 2.5 Visual assets (`docs/assets/`)

- Demo GIF of the live dashboard (served locally, driven, captured).
- Four dashboard tab screenshots.
- A 1280×640 social-preview image for the GitHub social card and LinkedIn link preview.
  (Setting the GitHub social preview itself is a repo-settings action for the author; the
  image is produced here and the step is documented.)

### 2.6 License

Add `LICENSE` — MIT. Copyright line names the author (confirm exact name/handle during
implementation; default to the GitHub handle otherwise).

---

## 3. Repository hygiene (de-tooling + cleanup)

- Rename `CLAUDE.md` → `ARCHITECTURE.md`; rewrite the opening so it reads as author-voiced
  architecture documentation (remove any tooling framing). The technical content — process
  topology, the two directions through the server, job lifecycle, storage, protocol
  invariants — is kept; it is strong architecture documentation.
- Sweep all tracked files for development-tooling references (any assistant/framework names)
  and remove or neutralise them.
- Rename `docs/superpowers/` → `docs/design/`; move the three existing design specs
  (`*-server-c2-design.md`, `*-probe-protocol-design.md`, `*-firmware-probe-design.md`)
  there. Docstrings cite spec **sections** (`spec §7.2`), not paths, so citations are
  unaffected; fix any file-path references that do point at the old directory.
- Ensure `.gitignore` excludes tooling/scratch/state dirs (`.claude/`, `.superpowers/`,
  `.pytest_cache/`) and stale databases (`network_monitor.db`, `instance/`).
- Delete stray shell-accident files (`onAlert({node_id`, `{`), the dead
  `templates/index.html d.html`, and the zero-byte `static/optional-custom.css`.
- **Keep `NOTICE`** and its third-party MIT attribution (ESP32-Bit-Pirate). This is a
  license obligation for reused code, independent of any tooling, and signals rigor.

---

## 4. Out of scope (backlog, not this pass)

Additional IDS detectors, a coverage badge, further feature code, and any offensive
transmit capability. The deauth transmit path stays unwired by design.

---

## 5. Success criteria

- A cold visitor understands what the project is and sees it working within ~20 seconds
  (hero + demo GIF).
- `docs/SECURITY.md` gives a security reader a threat model, working detectors, and a clear
  responsible-use stance.
- CI is green on a real run of the test suite; the badge is honest.
- `tests/test_attack_replay.py` passes and demonstrates the end-to-end detection pipeline.
- No tracked, portfolio-facing file references any development tooling.
- The repository reads as a single author's rigorous, self-contained project.
