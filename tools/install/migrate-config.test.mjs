// Tests for migrate-config.mjs and the PowerShell install scripts.
// Only synthetic keys are used. PowerShell tests run against temp dirs only (ruling F5).
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const MIGRATE = path.join(here, "migrate-config.mjs");
const NOT_RUNNING = "no-such-process-streammulticast";

const K_MAIN = "SYNTH-MAIN-KEY-live_1234567890_abcdef";
const K_AITUM = "SYNTH-AITUM-KEY-tt_9876543210_zyxwvu";
const K_OTHER = "SYNTH-OTHER-KEY-distinct_555555";
const K_DIFF = "SYNTH-DIFFERENT-KEY-same-server_777";
const ALL_KEYS = [K_MAIN, K_AITUM, K_OTHER, K_DIFF];

function tmp(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), `smulti-${prefix}-`));
}

function ep(name, key, extra = {}) {
  return {
    id: crypto.randomUUID(), name, server_url: "rtmp://example.invalid/live", stream_key: key,
    encoder_backend: 0, video_bitrate: 6000, keyframe_interval: 2, audio_bitrate: 160,
    orientation: 0, enabled: true, linked_to_main: true, sort_order: 1, ...extra,
  };
}

function fixture({ endpoints, mainServer = "auto", mainKey = K_MAIN, aitumOutputs, noAitum = false, compact = true }) {
  const dir = tmp("mig");
  const cfgDir = path.join(dir, "plugin_config", "streammulticast");
  fs.mkdirSync(cfgDir, { recursive: true });
  const config = path.join(cfgDir, "config.json");
  const body = { schema_version: 1, endpoints };
  fs.writeFileSync(config, compact ? JSON.stringify(body) : JSON.stringify(body, null, "\t") + "\n");
  const service = path.join(dir, "service.json");
  fs.writeFileSync(service, JSON.stringify({ type: "rtmp_common", settings: { service: "Twitch", server: mainServer, key: mainKey } }));
  const aitum = path.join(dir, "aitum.json");
  if (!noAitum) {
    fs.writeFileSync(aitum, JSON.stringify({
      outputs: aitumOutputs ?? [
        { enabled: true, type: "stream", name: "Vertical Stream", stream_server: "rtmps://push.tiktok.invalid:443/game", stream_key: K_AITUM },
        { enabled: false, type: "backtrack", name: "Vertical Backtrack" },
      ],
    }));
  }
  return { dir, config, service, aitum };
}

function run(f, extra = []) {
  const args = [MIGRATE, "--config", f.config, "--service", f.service, "--aitum", f.aitum, ...extra];
  const r = spawnSync(process.execPath, args, { encoding: "utf8" });
  return { code: r.status, out: (r.stdout ?? "") + (r.stderr ?? ""), stdout: r.stdout ?? "" };
}

function assertNoKeys(text) {
  for (const k of ALL_KEYS) assert.ok(!text.includes(k), "output must not contain a stream key");
}

test("duplicate of main: byte-equal key to main service", () => {
  const f = fixture({ endpoints: [ep("Twitch copy", K_MAIN)] });
  const r = run(f, ["--dry-run"]);
  assert.equal(r.code, 0);
  assert.match(r.stdout, /^Twitch copy: linked_to_main true -> false \(duplicate of main\)$/m);
  assertNoKeys(r.out);
});

test("duplicate of aitum: byte-equal key to Aitum output", () => {
  const f = fixture({ endpoints: [ep("Tiktok", K_AITUM)] });
  const r = run(f, ["--dry-run"]);
  assert.equal(r.code, 0);
  assert.match(r.stdout, /^Tiktok: linked_to_main true -> false \(duplicate of aitum:Vertical Stream\)$/m);
  assertNoKeys(r.out);
});

test("migrate_same_server_different_key_is_duplicate", () => {
  const f = fixture({ endpoints: [ep("Tiktok2", K_DIFF, { server_url: "RTMPS://push.tiktok.invalid:443/game/" })] });
  const r = run(f, ["--dry-run"]);
  assert.match(r.stdout, /^Tiktok2: linked_to_main true -> false \(duplicate of aitum:Vertical Stream\)$/m);
  assertNoKeys(r.out);
});

test("server of a disabled Aitum output does not count", () => {
  const f = fixture({
    endpoints: [ep("Tiktok3", K_DIFF, { server_url: "rtmps://push.tiktok.invalid:443/game" })],
    aitumOutputs: [{ enabled: false, type: "stream", name: "Off", stream_server: "rtmps://push.tiktok.invalid:443/game", stream_key: K_AITUM }],
  });
  const r = run(f, ["--dry-run"]);
  assert.match(r.stdout, /^Tiktok3: linked_to_main true \(distinct, unchanged\)$/m);
});

test("non-auto main server matches by server; auto never matches", () => {
  const eps = [ep("SameAsMain", K_DIFF, { server_url: "rtmp://live.example.invalid/app" })];
  let f = fixture({ endpoints: eps, mainServer: "rtmp://live.example.invalid/app" });
  assert.match(run(f, ["--dry-run"]).stdout, /^SameAsMain: linked_to_main true -> false \(duplicate of main\)$/m);
  f = fixture({ endpoints: eps, mainServer: "auto" });
  assert.match(run(f, ["--dry-run"]).stdout, /^SameAsMain: linked_to_main true \(distinct, unchanged\)$/m);
});

test("distinct endpoint stays linked", () => {
  const f = fixture({ endpoints: [ep("Kick", K_OTHER)] });
  const r = run(f, ["--dry-run"]);
  assert.match(r.stdout, /^Kick: linked_to_main true \(distinct, unchanged\)$/m);
});

test("already false is reported and untouched", () => {
  const f = fixture({ endpoints: [ep("Tiktok", K_AITUM, { linked_to_main: false })] });
  const r = run(f, ["--dry-run"]);
  assert.match(r.stdout, /^Tiktok: already false$/m);
});

test("empty keys never match by key", () => {
  const f = fixture({ endpoints: [ep("Empty", "")], mainKey: "" });
  const r = run(f, ["--dry-run"]);
  assert.match(r.stdout, /^Empty: linked_to_main true \(distinct, unchanged\)$/m);
});

test("dry-run does not modify the config", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN), ep("B", K_AITUM)] });
  const before = fs.readFileSync(f.config);
  const r = run(f, ["--dry-run"]);
  assert.equal(r.code, 0);
  assert.deepEqual(fs.readFileSync(f.config), before);
});

test("default (no flag) behaves as dry-run", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN)] });
  const before = fs.readFileSync(f.config);
  const r = run(f);
  assert.equal(r.code, 0);
  assert.deepEqual(fs.readFileSync(f.config), before);
});

test("migrate_apply_backs_up_and_flips_only_linked_to_main", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN), ep("B", K_AITUM), ep("C", K_OTHER)] });
  const original = fs.readFileSync(f.config);
  const backupRoot = tmp("bk");
  const r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-root", backupRoot]);
  assert.equal(r.code, 0, r.out);
  assertNoKeys(r.out);
  const after = JSON.parse(fs.readFileSync(f.config, "utf8"));
  assert.deepEqual(after.endpoints.map((e) => e.linked_to_main), [false, false, true]);
  assert.equal(after.schema_version, 1);
  assert.equal(after.endpoints[0].stream_key, K_MAIN);
  const dirs = fs.readdirSync(backupRoot);
  assert.equal(dirs.length, 1);
  assert.deepEqual(fs.readFileSync(path.join(backupRoot, dirs[0], "config.json")), original);
  const r2 = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-root", backupRoot]);
  assert.equal(r2.code, 0);
  assert.match(r2.stdout, /^A: already false$/m);
  assert.equal(fs.readdirSync(backupRoot).length, 1, "idempotent run makes no new backup");
});

test("--apply keeps a pretty-printed file pretty-printed", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN)], compact: false });
  const r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-root", tmp("bk")]);
  assert.equal(r.code, 0, r.out);
  const raw = fs.readFileSync(f.config, "utf8");
  assert.ok(raw.includes("\n\t"), "tab indentation preserved");
  assert.ok(raw.endsWith("\n"));
});

test("--apply with nothing to change does not write or back up", () => {
  const f = fixture({ endpoints: [ep("K", K_OTHER)] });
  const backupRoot = tmp("bk");
  const r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-root", backupRoot]);
  assert.equal(r.code, 0);
  assert.deepEqual(fs.readdirSync(backupRoot), []);
});

test("--apply refuses while the OBS process runs (exit 2) and leaves the file alone", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN)] });
  const before = fs.readFileSync(f.config);
  const r = run(f, ["--apply", "--process-name", "node", "--backup-root", tmp("bk")]);
  assert.equal(r.code, 2);
  assert.match(r.out, /OBS is running - close OBS first/);
  assert.deepEqual(fs.readFileSync(f.config), before);
});

test("--apply with --backup-dir requires an identical backup copy", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN)] });
  const bk = tmp("bk");
  let r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-dir", bk]);
  assert.equal(r.code, 1, "missing backup copy");
  fs.writeFileSync(path.join(bk, "config.json"), "different");
  r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-dir", bk]);
  assert.equal(r.code, 1, "non-identical backup copy");
  fs.copyFileSync(f.config, path.join(bk, "config.json"));
  r = run(f, ["--apply", "--process-name", NOT_RUNNING, "--backup-dir", bk]);
  assert.equal(r.code, 0, r.out);
  assert.equal(JSON.parse(fs.readFileSync(f.config, "utf8")).endpoints[0].linked_to_main, false);
});

test("missing Aitum file is tolerated; missing config or bad flags fail", () => {
  const f = fixture({ endpoints: [ep("A", K_MAIN)], noAitum: true });
  let r = run(f, ["--dry-run"]);
  assert.equal(r.code, 0);
  assert.match(r.stdout, /duplicate of main/);
  r = spawnSync(process.execPath, [MIGRATE, "--dry-run", "--config", path.join(f.dir, "nope.json")], { encoding: "utf8" });
  assert.equal(r.status, 1);
  r = spawnSync(process.execPath, [MIGRATE, "--dry-run", "--apply", "--config", f.config], { encoding: "utf8" });
  assert.equal(r.status, 1);
  r = spawnSync(process.execPath, [MIGRATE, "--dry-run"], { encoding: "utf8" });
  assert.equal(r.status, 1);
});

// ---------------------------------------------------------------- PowerShell scripts

const PS = ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File"];
function ps(script, args) {
  const r = spawnSync("powershell.exe", [...PS, path.join(here, script), ...args], { encoding: "utf8" });
  return { code: r.status, out: (r.stdout ?? "") + (r.stderr ?? "") };
}

function sha256(file) {
  return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

function makeSource({ withData = true } = {}) {
  const src = tmp("src");
  fs.writeFileSync(path.join(src, "streammulticast.dll"), "NEW-DLL");
  fs.writeFileSync(path.join(src, "streammulticast.pdb"), "NEW-PDB");
  if (withData) {
    fs.mkdirSync(path.join(src, "data", "locale"), { recursive: true });
    fs.writeFileSync(path.join(src, "data", "locale", "en-US.ini"), "NEW=1\n");
  }
  fs.writeFileSync(path.join(src, "migrate-config.mjs"), fs.readFileSync(MIGRATE));
  return src;
}

function makeInstall() {
  const root = tmp("obs");
  const plug = path.join(root, "obs-plugins", "64bit");
  const data = path.join(root, "data", "obs-plugins", "streammulticast", "locale");
  fs.mkdirSync(plug, { recursive: true });
  fs.mkdirSync(data, { recursive: true });
  fs.writeFileSync(path.join(plug, "streammulticast.dll"), "OLD-DLL");
  fs.writeFileSync(path.join(plug, "streammulticast.pdb"), "OLD-PDB");
  fs.writeFileSync(path.join(data, "en-US.ini"), "OLD=1\n");
  const cfgDir = path.join(root, "cfg");
  fs.mkdirSync(cfgDir, { recursive: true });
  const cfg = path.join(cfgDir, "config.json");
  fs.writeFileSync(cfg, JSON.stringify({ schema_version: 1, endpoints: [ep("Twitch copy", K_MAIN)] }));
  const svc = path.join(root, "service.json");
  fs.writeFileSync(svc, JSON.stringify({ type: "rtmp_common", settings: { server: "auto", key: K_MAIN } }));
  return { root, plug, data, cfgDir, cfg, svc };
}

function installArgs(src, inst, backupRoot, extra = [], processName = NOT_RUNNING) {
  return ["-Source", src, "-InstallDir", inst.root, "-BackupRoot", backupRoot, "-ConfigDir", inst.cfgDir,
    "-ServiceFile", inst.svc, "-AitumFile", path.join(inst.root, "no-aitum.json"), "-ProcessName", processName, ...extra];
}

test("install.ps1 -DryRun prints plan, touches nothing, exits 0", () => {
  const src = makeSource();
  const inst = makeInstall();
  const bk = tmp("bk");
  const r = ps("install.ps1", installArgs(src, inst, bk, ["-DryRun"]));
  assert.equal(r.code, 0, r.out);
  assert.match(r.out, /DRY RUN/);
  assert.match(r.out, /streammulticast-\d{8}-\d{6}/);
  assert.match(r.out, /streammulticast\.dll/);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "OLD-DLL");
  assert.deepEqual(fs.readdirSync(bk), []);
  assertNoKeys(r.out);
});

test("install.ps1 -DryRun still exits 0 while OBS runs", () => {
  const src = makeSource();
  const inst = makeInstall();
  const r = ps("install.ps1", installArgs(src, inst, tmp("bk"), ["-DryRun"], "node"));
  assert.equal(r.code, 0, r.out);
});

test("install_refuses_while_obs_runs", () => {
  const src = makeSource();
  const inst = makeInstall();
  const bk = tmp("bk");
  const r = ps("install.ps1", installArgs(src, inst, bk, [], "node"));
  assert.equal(r.code, 2, r.out);
  assert.match(r.out, /OBS is running - close OBS first/);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "OLD-DLL");
  assert.deepEqual(fs.readdirSync(bk), []);
});

test("install.ps1 rejects a source without a dll (exit 1)", () => {
  const src = tmp("empty");
  const inst = makeInstall();
  const r = ps("install.ps1", installArgs(src, inst, tmp("bk")));
  assert.equal(r.code, 1, r.out);
});

test("install.ps1 rejects a source whose SHA256SUMS does not match (exit 4)", () => {
  const src = makeSource();
  fs.writeFileSync(path.join(src, "SHA256SUMS"), `${"0".repeat(64)}  streammulticast.dll\n`);
  const inst = makeInstall();
  const bk = tmp("bk");
  const r = ps("install.ps1", installArgs(src, inst, bk));
  assert.equal(r.code, 4, r.out);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "OLD-DLL");
  assert.deepEqual(fs.readdirSync(bk), []);
});

test("install_backs_up_migrates_copies_and_rollback_restores", () => {
  const src = makeSource();
  const inst = makeInstall();
  const bk = tmp("bk");
  const oldCfg = fs.readFileSync(inst.cfg);

  const r = ps("install.ps1", installArgs(src, inst, bk));
  assert.equal(r.code, 0, r.out);
  assertNoKeys(r.out);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "NEW-DLL");
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.pdb"), "utf8"), "NEW-PDB");
  assert.equal(fs.readFileSync(path.join(inst.data, "en-US.ini"), "utf8"), "NEW=1\n");
  assert.equal(JSON.parse(fs.readFileSync(inst.cfg, "utf8")).endpoints[0].linked_to_main, false);
  const dirs = fs.readdirSync(bk).filter((d) => /^streammulticast-\d{8}-\d{6}$/.test(d));
  assert.equal(dirs.length, 1);
  const b = path.join(bk, dirs[0]);
  assert.equal(fs.readFileSync(path.join(b, "streammulticast.dll"), "utf8"), "OLD-DLL");
  assert.equal(fs.readFileSync(path.join(b, "streammulticast.pdb"), "utf8"), "OLD-PDB");
  assert.deepEqual(fs.readFileSync(path.join(b, "plugin_config", "streammulticast", "config.json")), oldCfg);
  assert.equal(fs.readFileSync(path.join(b, "data", "locale", "en-US.ini"), "utf8"), "OLD=1\n");

  const common = ["-BackupRoot", bk, "-InstallDir", inst.root, "-ConfigDir", inst.cfgDir, "-ProcessName", NOT_RUNNING];
  const d = ps("rollback.ps1", [...common, "-DryRun"]);
  assert.equal(d.code, 0, d.out);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "NEW-DLL");

  const rr = ps("rollback.ps1", common);
  assert.equal(rr.code, 0, rr.out);
  assertNoKeys(rr.out);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "OLD-DLL");
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.pdb"), "utf8"), "OLD-PDB");
  assert.equal(fs.readFileSync(path.join(inst.data, "en-US.ini"), "utf8"), "OLD=1\n");
  assert.deepEqual(fs.readFileSync(inst.cfg), oldCfg);
});

test("rollback.ps1 -From picks a specific backup and refuses while OBS runs", () => {
  const src = makeSource();
  const inst = makeInstall();
  const bk = tmp("bk");
  assert.equal(ps("install.ps1", installArgs(src, inst, bk)).code, 0);
  const dir = path.join(bk, fs.readdirSync(bk)[0]);
  const base = ["-From", dir, "-InstallDir", inst.root, "-ConfigDir", inst.cfgDir, "-BackupRoot", bk];
  const running = ps("rollback.ps1", [...base, "-ProcessName", "node"]);
  assert.equal(running.code, 2, running.out);
  assert.match(running.out, /OBS is running - close OBS first/);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "NEW-DLL");
  const ok = ps("rollback.ps1", [...base, "-ProcessName", NOT_RUNNING]);
  assert.equal(ok.code, 0, ok.out);
  assert.equal(fs.readFileSync(path.join(inst.plug, "streammulticast.dll"), "utf8"), "OLD-DLL");
});

test("rollback of a first install removes what the install added", () => {
  const src = makeSource();
  const inst = makeInstall();
  fs.rmSync(path.join(inst.plug, "streammulticast.dll"));
  fs.rmSync(path.join(inst.plug, "streammulticast.pdb"));
  fs.rmSync(path.join(inst.root, "data"), { recursive: true });
  const bk = tmp("bk");
  assert.equal(ps("install.ps1", installArgs(src, inst, bk)).code, 0);
  assert.ok(fs.existsSync(path.join(inst.plug, "streammulticast.dll")));
  const rr = ps("rollback.ps1", ["-BackupRoot", bk, "-InstallDir", inst.root, "-ConfigDir", inst.cfgDir, "-ProcessName", NOT_RUNNING]);
  assert.equal(rr.code, 0, rr.out);
  assert.ok(!fs.existsSync(path.join(inst.plug, "streammulticast.dll")));
  assert.ok(!fs.existsSync(path.join(inst.root, "data", "obs-plugins", "streammulticast")));
});

test("install.ps1 with a non-writable plugin dir exits 3 with the elevated command, config untouched", () => {
  const src = makeSource();
  const inst = makeInstall();
  fs.rmSync(path.join(inst.root, "obs-plugins"), { recursive: true });
  fs.writeFileSync(path.join(inst.root, "obs-plugins"), "a file, not a directory");
  const before = fs.readFileSync(inst.cfg);
  const r = ps("install.ps1", installArgs(src, inst, tmp("bk")));
  assert.equal(r.code, 3, r.out);
  assert.match(r.out, /Start-Process powershell -Verb RunAs -ArgumentList/);
  assert.deepEqual(fs.readFileSync(inst.cfg), before, "config untouched");
});

// ---------------------------------------------------------------- stage.ps1

test("stage_writes_version_and_verifiable_sha256sums", () => {
  const build = tmp("build");
  fs.writeFileSync(path.join(build, "streammulticast.dll"), "BUILD-DLL");
  fs.writeFileSync(path.join(build, "streammulticast.pdb"), "BUILD-PDB");
  const out = tmp("stage");
  const sha = "0123456789abcdef0123456789abcdef01234567";
  const r = ps("stage.ps1", ["-Build", build, "-OutRoot", out, "-Sha", sha]);
  assert.equal(r.code, 0, r.out);
  const dir = path.join(out, sha);
  const files = ["streammulticast.dll", "streammulticast.pdb", "data/locale/en-US.ini", "install.ps1", "rollback.ps1", "migrate-config.mjs", "VERSION", "SHA256SUMS"];
  for (const f of files) assert.ok(fs.existsSync(path.join(dir, f)), `missing ${f}`);
  assert.equal(fs.readFileSync(path.join(dir, "VERSION"), "utf8").trim(), sha);
  const sums = fs.readFileSync(path.join(dir, "SHA256SUMS"), "utf8");
  assert.ok(!sums.includes("\r"), "LF endings");
  assert.ok(!sums.includes("SHA256SUMS"), "does not list itself");
  const chk = spawnSync("sha256sum", ["-c", "SHA256SUMS"], { cwd: dir, encoding: "utf8" });
  assert.equal(chk.status, 0, chk.stdout + chk.stderr);
  assert.equal(sha256(path.join(dir, "streammulticast.dll")), sha256(path.join(build, "streammulticast.dll")));
  const again = ps("stage.ps1", ["-Build", build, "-OutRoot", out, "-Sha", sha]);
  assert.notEqual(again.code, 0);
  const inst = makeInstall();
  const dry = ps("install.ps1", installArgs(dir, inst, tmp("bk"), ["-DryRun"]));
  assert.equal(dry.code, 0, dry.out);
  assert.match(dry.out, /SHA256SUMS verified/);
});

test("stage.ps1 fails when the build dir has no dll", () => {
  const r = ps("stage.ps1", ["-Build", tmp("nobuild"), "-OutRoot", tmp("stage"), "-Sha", "abc"]);
  assert.notEqual(r.code, 0);
});

test("stage.ps1 without -Sha uses git HEAD of the repo", () => {
  const build = tmp("build");
  fs.writeFileSync(path.join(build, "streammulticast.dll"), "D");
  fs.writeFileSync(path.join(build, "streammulticast.pdb"), "P");
  const out = tmp("stage");
  const head = spawnSync("git", ["rev-parse", "HEAD"], { cwd: here, encoding: "utf8" }).stdout.trim();
  const r = ps("stage.ps1", ["-Build", build, "-OutRoot", out]);
  assert.equal(r.code, 0, r.out);
  assert.equal(fs.readFileSync(path.join(out, head, "VERSION"), "utf8").trim(), head);
});
