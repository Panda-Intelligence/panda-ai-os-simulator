// Extracted from the reviewed Murphy browser packaging seam; now owned upstream.
export function patchQemuScriptForBrowserWorker(source) {
  const normalizedSource = source.replaceAll('payloadLen"+" tSelf=', 'payloadLen+" tSelf=');
  const legacyIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module.print==="function")Module.print("[BRIDGE] ch="+channel+" ptr="+payloadPtr+" len="+payloadLen+" tSelf="+typeof self._mofeiWasmIpcSend);if(typeof self._mofeiWasmIpcSend==="function"){self._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}else{Module.print("[BRIDGE-SKIP] self._mofeiWasmIpcSend missing")}}';
  const moduleDotIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module._mofeiWasmIpcSend==="function"){Module._mofeiWasmIpcSend(channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}';
  const moduleBracketIpcHook =
    'function _mofei_wasm_ipc_send(channel,flags,payloadPtr,payloadLen){if(typeof Module["_mofeiWasmIpcSend"]==="function"){Module["_mofeiWasmIpcSend"](channel>>>0,flags>>>0,payloadPtr>>>0,payloadLen>>>0)}}';
  const knownHandlersPattern = /var knownHandlers=\[([^\]]*)\];/;

  let patchedSource = normalizedSource;
  if (patchedSource.includes(legacyIpcHook)) {
    patchedSource = patchedSource.replace(legacyIpcHook, moduleBracketIpcHook);
  } else if (patchedSource.includes(moduleDotIpcHook)) {
    patchedSource = patchedSource.replace(moduleDotIpcHook, moduleBracketIpcHook);
  } else if (!patchedSource.includes(moduleBracketIpcHook)) {
    throw new Error("qemu-wasm runtime IPC hook shape changed; update build-simulator-web.mjs patch");
  }

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

  return patchedSource
    .replace(knownHandlersPattern, `var knownHandlers=[${knownHandlers.join(",")}];`)
    .replace(/export\s+default\s+Module\s*;?/g, "export default Module;");
}
