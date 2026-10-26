import test from "node:test";
import assert from "node:assert/strict";
import { createHash, webcrypto } from "node:crypto";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";
import ts from "typescript";

const root = new URL("../", import.meta.url);
const source = readFileSync(new URL("src/posDevBridge.ts", root), "utf8")
  .replace(/^import[^\n]+\n/gm, "")
  .replace(/= browserSimulatorRuntimeHost/g, "= undefined");
const javascript = ts.transpileModule(`function createSimulatedLocation() { return {name:"London",timezone:"Europe%2FLondon",latitude:51.5074,longitude:-0.1278}; }\n${source}`, {
  compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 },
}).outputText;
const { createPosConsoleClient, createPosRuntimeHealthMonitor, applyPosDevBuild, isPosDevBuild } = await import("data:text/javascript;base64," + Buffer.from(javascript).toString("base64"));
if (!globalThis.crypto) globalThis.crypto = webcrypto;

function consoleHost(respond) {
  const listeners = new Set(), calls = [];
  return {
    calls,
    async subscribeSerialLog(handler) { listeners.add(handler); return () => listeners.delete(handler); },
    async sendSerialLine(line) {
      calls.push(line);
      await respond(line, (value, fragmented = false) => {
        const message = `POS_APP:${value.requestId}:${JSON.stringify(value)}\n`;
        for (const listener of listeners) {
          if (fragmented) { listener(message.slice(0, 17)); listener(message.slice(17)); }
          else listener(message);
        }
      });
    },
    listenerCount() { return listeners.size; },
  };
}

test("console client waits for guest result and polls a queued operation by the same ID", async () => {
  const host = consoleHost(async (line, emit) => {
    const [, , requestId, operation] = line.split(":");
    if (operation === "install") emit({ schemaVersion:1, requestId, operation, state:"queued", ok:true, terminal:false }, true);
    else emit({ schemaVersion:1, requestId, operation:"install", state:"completed", ok:true, terminal:true, appId:"ai.pandacat.app.test", version:"1.0.0" });
  });
  const result = await createPosConsoleClient(host, 2000)("install", "/PandaDev/" + "a".repeat(64) + ".pap");
  assert.equal(result.appId, "ai.pandacat.app.test");
  assert.equal(host.calls.length, 2);
  assert.equal(host.calls[0].split(":")[2], host.calls[1].split(":")[2]);
  assert.match(host.calls[1], /:status$/);
  assert.equal(host.listenerCount(), 0);
});

test("console send acknowledgement alone cannot be reported as successful installation", async () => {
  const host = consoleHost(async () => {});
  await assert.rejects(createPosConsoleClient(host, 35)("install", "/PandaDev/" + "a".repeat(64) + ".pap"), /install_timeout/);
  assert.equal(host.listenerCount(), 0);
});

test("lost queued acknowledgement polls status without replaying the install command", async () => {
  const host = consoleHost(async (line, emit) => {
    const [, , requestId, operation] = line.split(":");
    if (operation === "install") return;
    emit({schemaVersion:1,requestId,operation:"install",state:"completed",terminal:true,ok:true,appId:"ai.pandacat.app.test",version:"1.0.0"});
  });
  const result = await createPosConsoleClient(host, 2200)("install", "/PandaDev/" + "a".repeat(64) + ".pap");
  assert.equal(result.state,"completed");
  assert.equal(host.calls.filter((line)=>line.includes(":install:")).length,1);
  assert.match(host.calls[1],/:status$/);
});

test("explicit worker refusals back off with new IDs while an admitted install keeps its ID", async () => {
  const refusals = ["worker_busy", "busy"];
  const admitted = new Set();
  const host = consoleHost(async (line, emit) => {
    const [, , requestId, operation] = line.split(":");
    if (operation === "install") {
      const error = refusals.shift();
      if (error) emit({schemaVersion:1,requestId,operation,state:"failed",terminal:true,ok:false,error});
      else { admitted.add(requestId); emit({schemaVersion:1,requestId,operation,state:"queued",terminal:false,ok:true}); }
    } else {
      assert.ok(admitted.has(requestId));
      emit({schemaVersion:1,requestId,operation:"install",state:"completed",terminal:true,ok:true,appId:"ai.pandacat.app.test",version:"1.0.0"});
    }
  });
  const result = await createPosConsoleClient(host, 2000)("install", "/PandaDev/" + "a".repeat(64) + ".pap");
  const attempts = host.calls.filter((line)=>line.includes(":install:"));
  assert.equal(result.state,"completed");
  assert.equal(attempts.length,3);
  assert.equal(new Set(attempts.map((line)=>line.split(":")[2])).size,3);
  assert.equal(admitted.size,1);
  assert.equal(host.calls.at(-1).split(":")[2],[...admitted][0]);
  assert.equal(host.listenerCount(),0);
});

test("persistent worker contention stays within the original operation deadline", async () => {
  const host = consoleHost(async (line, emit) => {
    const [, , requestId,operation] = line.split(":");
    emit({schemaVersion:1,requestId,operation,state:"failed",terminal:true,ok:false,error:"worker_busy"});
  });
  const started = Date.now();
  await assert.rejects(createPosConsoleClient(host, 120)("install", "/PandaDev/" + "a".repeat(64) + ".pap"), /install_timeout: worker_busy/);
  assert.ok(Date.now()-started<350);
  assert.equal(host.calls.length,1);
  assert.equal(host.listenerCount(),0);
});

test("late status-not-found responses cannot overwrite a matching terminal capability result", async () => {
  const host = consoleHost(async (line, emit) => {
    const [, , requestId,operation] = line.split(":");
    emit({schemaVersion:1,requestId,operation,state:"completed",terminal:true,ok:true,capabilities:{install:true,open:true}});
    emit({schemaVersion:1,requestId,operation:"status",state:"failed",terminal:true,ok:false,error:"request_not_found"});
  });
  const result = await createPosConsoleClient(host, 100)("capabilities");
  assert.equal(result.operation,"capabilities");
  assert.deepEqual(result.capabilities,{install:true,open:true});
  assert.equal(host.calls.length,1);
});

test("guest verifier failure propagates unchanged and unsubscribes", async () => {
  const host = consoleHost(async (line, emit) => {
    const requestId = line.split(":")[2];
    emit({ schemaVersion:1, requestId, operation:"install", state:"failed", terminal:true, ok:false, error:"package_v2_device_verifier_unavailable" });
  });
  await assert.rejects(createPosConsoleClient(host, 100)("install", "/PandaDev/" + "a".repeat(64) + ".pap"), /package_v2_device_verifier_unavailable/);
  assert.equal(host.listenerCount(), 0);
});

test("Runtime health updates require exact open request and package identity, including buffered early failure", async () => {
  let emit;
  const updates = [];
  const host = {async subscribeSerialLog(callback){emit=callback;return()=>{emit=undefined;};}};
  const monitor = await createPosRuntimeHealthMonitor(host,(result)=>updates.push(result));
  const expected={requestId:"opened_1",appId:"ai.pandacat.app.test",version:"1.0.0",packageDigest:"sha256:"+"a".repeat(64)};
  const send=(overrides={})=>{const value={schemaVersion:1,operation:"runtime",state:"failed",ok:false,terminal:true,error:"runtime_deadline_exceeded",...expected,...overrides};emit(`POS_APP:${value.requestId}:${JSON.stringify(value)}\n`);};
  send();
  assert.equal(updates.length,0);
  monitor.watch(expected);
  assert.equal(updates.length,1);
  assert.equal(updates[0].error,"runtime_deadline_exceeded");
  send({requestId:"stale_open"});
  send({packageDigest:"sha256:"+"b".repeat(64)});
  send({appId:"other.app"});
  assert.equal(updates.length,1);
  send({state:"running",ok:true,terminal:false,error:""});
  send({state:"stopped",ok:true,terminal:true,error:""});
  assert.equal(updates.at(-2).state,"running");
  assert.equal(updates.at(-1).state,"stopped");
  monitor.dispose();
  assert.equal(emit,undefined);
});

test("session cancellation releases an outstanding firmware response listener", async () => {
  const host = consoleHost(async () => {});
  const controller = new AbortController();
  const pending = createPosConsoleClient(host, 180000, controller.signal)("capabilities");
  setTimeout(() => controller.abort(), 5);
  await assert.rejects(pending, /pos_runtime_cancelled/);
  assert.equal(host.listenerCount(), 0);
});

test("browser validates archive digest before stopping the previous guest", async () => {
  let stopped = false;
  const build = { schemaVersion:1, revision:1, appId:"ai.pandacat.app.test", version:"1.0.0", board:"mofei", packageDigest:"sha256:" + "0".repeat(64), packageUrl:"/__pos/package/1.pap" };
  assert.equal(isPosDevBuild(build), true);
  assert.equal(isPosDevBuild({...build, packageUrl:"https://other.invalid/package.pap"}), false);
  await assert.rejects(applyPosDevBuild(build, { async stopSim() { stopped = true; } }, async () => new Response("signed archive")), /digest_mismatch/);
  assert.equal(stopped, false);
});

test("browser stages exact package bytes, then requires real capability/install/open identities", async () => {
  const bytes = new TextEncoder().encode("signed exact artifact");
  const digest = createHash("sha256").update(bytes).digest("hex");
  const steps = [];
  const host = consoleHost(async (line, emit) => {
    const [, , requestId, operation] = line.split(":");
    steps.push(operation);
    emit({ schemaVersion:1, requestId, operation, state:"completed", terminal:true, ok:true,
      capabilities:{install:true,open:true}, appId:"ai.pandacat.app.test", version:"1.0.0" });
  });
  Object.assign(host, {
    async chooseFirmware(board) { assert.equal(board,"mofei"); return "firmware.bin"; },
    async stopSim() { steps.push("stop"); return "stopped"; },
    async createSdDirectory(board,path) { assert.equal(board,"mofei"); assert.equal(path,"/PandaDev"); steps.push("mkdir"); },
    async writeSdFile(board,path,actual) { assert.equal(board,"mofei"); assert.equal(path,`/PandaDev/${digest}.pap`); assert.deepEqual(actual,bytes); steps.push("stage"); },
    async startSim(board,firmware,location) { assert.equal(board,"mofei"); assert.equal(firmware,"firmware.bin"); assert.equal(location.name,"London"); steps.push("start"); return "started"; },
  });
  await applyPosDevBuild({ schemaVersion:1,revision:1,appId:"ai.pandacat.app.test",version:"1.0.0",board:"mofei",packageDigest:"sha256:"+digest,packageUrl:"/__pos/package/1.pap" }, host, async () => new Response(bytes));
  assert.deepEqual(steps,["stop","mkdir","stage","start","capabilities","install","open"]);
});

for (const failure of ["stage", "open"]) test(`failed ${failure} restores prior exact package through real install/open protocol`, async () => {
  const original = new TextEncoder().encode("previous signed archive"), changed = new TextEncoder().encode("new signed archive");
  const info = (bytes,revision) => ({schemaVersion:1,revision,appId:"ai.pandacat.app.test",version:"1.0.0",board:"mofei",packageDigest:"sha256:"+createHash("sha256").update(bytes).digest("hex"),packageUrl:`/__pos/package/${revision}.pap`});
  let staged = 0, opened = 0, running = false;
  const written = [], installs = [];
  const host = consoleHost(async (line, emit) => {
    const [, , requestId, operation,...args] = line.split(":");
    if (operation === "install") installs.push(args.join(":"));
    if (operation === "open" && ++opened === 2 && failure === "open") {
      emit({schemaVersion:1,requestId,operation,state:"failed",terminal:true,ok:false,error:"launch_failed"}); return;
    }
    emit({schemaVersion:1,requestId,operation,state:"completed",terminal:true,ok:true,capabilities:{install:true,open:true},appId:"ai.pandacat.app.test",version:"1.0.0"});
  });
  Object.assign(host,{
    async chooseFirmware() {return "firmware.bin";},
    async stopSim() {running=false;return "stopped";},
    async createSdDirectory() {},
    async writeSdFile(_board,path,bytes) { if (++staged===2 && failure==="stage") throw new Error("disk_full"); written.push({path,bytes:Uint8Array.from(bytes)}); },
    async startSim() {running=true;return "started";},
  });
  const previous = await applyPosDevBuild(info(original,1),host,async()=>new Response(original));
  await assert.rejects(applyPosDevBuild(info(changed,2),host,async()=>new Response(changed),undefined,previous),failure==="stage"?/disk_full/:/launch_failed/);
  assert.equal(running,true);
  assert.deepEqual(written.at(-1).bytes,original);
  assert.equal(installs.at(-1),`/PandaDev/${info(original,1).packageDigest.slice(7)}.pap`);
  assert.equal(host.calls.at(-1).split(":")[3],"open");
});

const workerSource = readFileSync(new URL("src/browserSimulatorRuntimeWorker.js", root), "utf8").replace(/^import[^\n]+\n/gm, "");
const serial = runInNewContext(workerSource + "\n;({sendSerialLine,setRunning(value){running=value;},read(){return qemuPty.read(1024);}})", {
  Uint8Array, ArrayBuffer, DataView, TextEncoder, TextDecoder, Date, console,
  postMessage() {}, addEventListener() {},
});

test("actual worker serial transport forwards one bounded line into the real PTY queue", () => {
  assert.equal(serial.sendSerialLine("ping"),"browser_serial_runtime_not_running");
  serial.setRunning(true);
  assert.equal(serial.sendSerialLine("ping\nother"),"browser_serial_line_invalid");
  assert.equal(serial.sendSerialLine("x".repeat(513)),"browser_serial_line_invalid");
  assert.equal(serial.sendSerialLine("CMD:POS_APP:test:capabilities"),undefined);
  assert.equal(new TextDecoder().decode(Uint8Array.from(serial.read())),"CMD:POS_APP:test:capabilities\n");
});
