#!/usr/bin/env node
// StreamMulticast config migration: unlink endpoints that would push to the same
// channel as the main OBS stream or an Aitum output ("double push").
//
// Duplicate rule (ruling F1): an endpoint is a duplicate when
//   - its non-empty stream key is byte-equal to the main service key or to any Aitum stream output key, OR
//   - its normalized server equals the server of an ENABLED Aitum stream output,
//     or the main service server when that is not "auto".
// Keys are compared in memory only and are never printed.
//
// Usage:
//   node migrate-config.mjs --config <config.json> [--dry-run | --apply]
//        [--aitum <aitum.json>] [--service <service.json>]
//        [--backup-dir <dir> | --backup-root <dir>] [--process-name <name>]
// Default is a dry run. --apply refuses while OBS runs (exit 2) and writes only after a
// verified backup copy of the config exists.
// Exit codes: 0 ok, 1 error, 2 OBS running.

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

class UsageError extends Error {}

export function normalizeServer(url) {
  if (typeof url !== "string") return "";
  const m = /^\s*([a-z][a-z0-9+.-]*):\/\/([^/:?#\s]+)(?::(\d+))?([^?#\s]*)/i.exec(url);
  if (!m) return "";
  const scheme = m[1].toLowerCase();
  const host = m[2].toLowerCase();
  const defaults = { rtmp: "1935", rtmps: "443" };
  const port = m[3] ?? defaults[scheme] ?? "";
  const pth = m[4].replace(/\/+$/, "");
  return `${host}:${port}${pth}`;
}

// Returns decisions for every endpoint, without mutating `config`.
export function plan(config, main, aitumOutputs) {
  const mainKey = typeof main?.key === "string" ? main.key : "";
  const mainServer = typeof main?.server === "string" && main.server.toLowerCase() !== "auto" ? normalizeServer(main.server) : "";
  const outs = (aitumOutputs ?? []).filter((o) => o && o.type === "stream");
  const endpoints = Array.isArray(config?.endpoints) ? config.endpoints : [];
  return endpoints.map((e, index) => {
    const name = String(e?.name ?? e?.id ?? "?");
    if (e?.linked_to_main === false) return { index, name, action: "already-false" };
    if (e?.linked_to_main !== true) return { index, name, action: "not-linked" };
    const key = typeof e.stream_key === "string" ? e.stream_key : "";
    const server = normalizeServer(e.server_url);
    let reason = null;
    if (key && mainKey && key === mainKey) reason = "main";
    if (!reason && mainServer && server && server === mainServer) reason = "main";
    if (!reason) {
      for (const o of outs) {
        const okey = typeof o.stream_key === "string" ? o.stream_key : "";
        const byKey = key && okey && key === okey;
        const byServer = o.enabled === true && server && normalizeServer(o.stream_server) === server;
        if (byKey || byServer) { reason = `aitum:${o.name ?? "?"}`; break; }
      }
    }
    return reason ? { index, name, action: "unlink", reason } : { index, name, action: "distinct" };
  });
}

export function describe(d) {
  switch (d.action) {
    case "unlink": return `${d.name}: linked_to_main true -> false (duplicate of ${d.reason})`;
    case "already-false": return `${d.name}: already false`;
    case "not-linked": return `${d.name}: linked_to_main not set (unchanged)`;
    default: return `${d.name}: linked_to_main true (distinct, unchanged)`;
  }
}

function readJson(file) {
  const raw = fs.readFileSync(file, "utf8");
  const bom = raw.charCodeAt(0) === 0xfeff;
  const text = bom ? raw.slice(1) : raw;
  return { data: JSON.parse(text), text, bom };
}

function serializeLike(data, text, bom) {
  let indent;
  const m = /^\s*\{\s*\r?\n([ \t]+)"/.exec(text);
  if (m) indent = m[1];
  let out = JSON.stringify(data, null, indent);
  if (/\r?\n\s*$/.test(text) && m) out += /\r\n\s*$/.test(text) ? "\r\n" : "\n";
  return (bom ? "\uFEFF" : "") + out;
}

function isRunning(name) {
  const image = /\.[a-z]+$/i.test(name) ? name : `${name}.exe`;
  const r = spawnSync("tasklist", ["/FI", `IMAGENAME eq ${image}`, "/NH", "/FO", "CSV"], { encoding: "utf8" });
  if (r.error || r.status !== 0) throw new Error("cannot query the process list (tasklist failed)");
  return r.stdout.toLowerCase().includes(`"${image.toLowerCase()}"`);
}

function stamp() {
  const d = new Date();
  const p = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

function parseArgs(argv) {
  const o = { dryRun: false, apply: false, processName: "obs64" };
  const need = (i) => {
    if (i + 1 >= argv.length) throw new UsageError(`missing value for ${argv[i]}`);
    return argv[i + 1];
  };
  for (let i = 0; i < argv.length; i++) {
    switch (argv[i]) {
      case "--dry-run": o.dryRun = true; break;
      case "--apply": o.apply = true; break;
      case "--config": o.config = need(i++); break;
      case "--aitum": o.aitum = need(i++); break;
      case "--service": o.service = need(i++); break;
      case "--backup-dir": o.backupDir = need(i++); break;
      case "--backup-root": o.backupRoot = need(i++); break;
      case "--process-name": o.processName = need(i++); break;
      default: throw new UsageError(`unknown argument ${argv[i]}`);
    }
  }
  if (!o.config) throw new UsageError("--config <path> is required");
  if (o.dryRun && o.apply) throw new UsageError("--dry-run and --apply are mutually exclusive");
  return o;
}

// Active profile folder from user.ini (ProfileDir=...), or null.
function activeProfileDir(obsDir) {
  try {
    const ini = fs.readFileSync(path.join(obsDir, "user.ini"), "utf8");
    const m = /^ProfileDir=(.+?)\r?$/m.exec(ini);
    return m ? path.join(obsDir, "basic", "profiles", m[1]) : null;
  } catch { return null; }
}

function loadContext(o) {
  const notes = [];
  // <obs>/plugin_config/streammulticast/config.json
  const obsDir = path.resolve(path.dirname(o.config), "..", "..");
  const profile = activeProfileDir(obsDir);
  const aitumPath = o.aitum ?? (profile ? path.join(profile, "aitum.json") : null);
  const servicePath = o.service ?? (profile ? path.join(profile, "service.json") : null);

  let main = null;
  if (servicePath && fs.existsSync(servicePath)) {
    main = readJson(servicePath).data?.settings ?? null;
  } else {
    notes.push("main service: not found (main comparison skipped)");
  }
  let outputs = [];
  if (aitumPath && fs.existsSync(aitumPath)) {
    outputs = readJson(aitumPath).data?.outputs ?? [];
  } else {
    notes.push("aitum config: not found (aitum comparison skipped)");
  }
  return { main, outputs, notes };
}

export function main(argv) {
  let o;
  try { o = parseArgs(argv); } catch (e) {
    console.error(`error: ${e.message}`);
    return 1;
  }
  let cfg;
  try { cfg = readJson(o.config); } catch (e) {
    console.error(`error: cannot read config (${e.code ?? e.name})`);
    return 1;
  }
  let ctx;
  try { ctx = loadContext(o); } catch (e) {
    console.error(`error: cannot read main/aitum settings (${e.code ?? e.name})`);
    return 1;
  }

  const decisions = plan(cfg.data, ctx.main, ctx.outputs);
  console.log(`${o.apply ? "APPLY" : "DRY RUN"}: ${path.basename(o.config)}, ${decisions.length} endpoint(s)`);
  for (const n of ctx.notes) console.log(`note: ${n}`);
  for (const d of decisions) console.log(describe(d));
  const toChange = decisions.filter((d) => d.action === "unlink");

  if (!o.apply) {
    console.log(toChange.length ? `would unlink ${toChange.length} endpoint(s); nothing written` : "nothing to change");
    return 0;
  }
  if (toChange.length === 0) {
    console.log("nothing to change; no backup made, no file written");
    return 0;
  }

  try {
    if (isRunning(o.processName)) {
      console.error("OBS is running - close OBS first");
      return 2;
    }
  } catch (e) {
    console.error(`error: ${e.message}`);
    return 1;
  }

  // Backup check: either verify a caller-provided backup, or make one.
  let backupDir = o.backupDir;
  if (backupDir) {
    // Accepts a flat copy (<dir>/config.json) or the install.ps1 layout.
    const candidates = [path.join(backupDir, "config.json"), path.join(backupDir, "plugin_config", "streammulticast", "config.json")];
    let ok = false;
    for (const copy of candidates) {
      try { if (fs.readFileSync(copy).equals(fs.readFileSync(o.config))) { ok = true; break; } } catch { /* try next */ }
    }
    if (!ok) {
      console.error(`error: --backup-dir must contain an identical config.json copy (${candidates[0]})`);
      return 1;
    }
  } else {
    const root = o.backupRoot ?? "E:\\Backups";
    backupDir = path.join(root, `streammulticast-config-${stamp()}`);
    for (let n = 2; fs.existsSync(backupDir); n++) backupDir = path.join(root, `streammulticast-config-${stamp()}-${n}`);
    fs.mkdirSync(backupDir, { recursive: true });
    fs.copyFileSync(o.config, path.join(backupDir, "config.json"));
    if (!fs.readFileSync(path.join(backupDir, "config.json")).equals(fs.readFileSync(o.config))) {
      console.error("error: backup verification failed; config not modified");
      return 1;
    }
  }
  console.log(`backup: ${backupDir}`);

  for (const d of toChange) cfg.data.endpoints[d.index].linked_to_main = false;
  const tmpFile = `${o.config}.migrate-tmp`;
  try {
    fs.writeFileSync(tmpFile, serializeLike(cfg.data, cfg.text, cfg.bom));
    JSON.parse(fs.readFileSync(tmpFile, "utf8").replace(/^\uFEFF/, ""));
    fs.renameSync(tmpFile, o.config);
  } catch (e) {
    try { fs.rmSync(tmpFile, { force: true }); } catch { /* ignore */ }
    console.error(`error: write failed (${e.code ?? e.name}); config unchanged`);
    return 1;
  }
  console.log(`unlinked ${toChange.length} endpoint(s)`);
  return 0;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exitCode = main(process.argv.slice(2));
}
