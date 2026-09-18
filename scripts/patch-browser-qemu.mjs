import ts from "typescript";
import { patchWasmContinuations } from "./patch-wasm-continuations.mjs";
// Extracted from the reviewed Murphy browser packaging seam; now owned upstream.
export function patchQemuScriptForBrowserWorker(source) {
  const normalizedSource = source.replaceAll('payloadLen"+" tSelf=', 'payloadLen+" tSelf=');
  const legacyIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module.print==="function")Module.print("[BRIDGE] ch="+channel+" ptr="+payloadPtr+" len="+payloadLen+" tSelf="+typeof self._mofeiWasmIpcSend);if(typeof self._mofeiWasmIpcSend==="function"){self._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}else{Module.print("[BRIDGE-SKIP] self._mofeiWasmIpcSend missing")}}';
  const moduleDotIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module._mofeiWasmIpcSend==="function"){Module._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}';
  const moduleBracketIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module["_mofeiWasmIpcSend"]==="function"){Module["_mofeiWasmIpcSend"](channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}';
  const knownHandlersPattern = /var knownHandlers\s*=\s*\[([^\]]*)\];/;

  const file = ts.createSourceFile("qemu.js", normalizedSource, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  const hooks = [];
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name?.text === "_mofei_wasm_ipc_send") hooks.push(node);
    ts.forEachChild(node, visit);
  }
  visit(file);
  if (hooks.length !== 1) throw new Error("qemu-wasm runtime IPC hook count changed");
  const hook = hooks[0], body = hook.getText(file).replace(/\s+/g, "");
  const known = [legacyIpcHook, moduleDotIpcHook, moduleBracketIpcHook].map(text => text.replace(/\s+/g, ""));
  if (!known.includes(body)) throw new Error("qemu-wasm runtime IPC hook shape changed");
  let patchedSource = normalizedSource.slice(0, hook.getStart(file)) + moduleBracketIpcHook + normalizedSource.slice(hook.end);

  const knownHandlersMatch = patchedSource.match(knownHandlersPattern);
  if (!knownHandlersMatch) {
    throw new Error("qemu-wasm runtime knownHandlers shape changed; update build-simulator-web.mjs patch");
  }
  const knownHandlers = knownHandlersMatch[1]
    .split(",")
    .map((handler) => handler.trim())
    .filter(Boolean);
  if (!knownHandlers.includes('"_mofeiWasmIpcSend"')) {
    knownHandlers.push('"_mofeiWasmIpcSend"');
  }

  return patchWasmContinuations(patchedSource
    .replace(knownHandlersPattern, `var knownHandlers=[${knownHandlers.join(",")}];`)
    .replace(/export\s+default\s+Module\s*;?/g, "export default Module;"));
}
