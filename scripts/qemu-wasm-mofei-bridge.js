mergeInto(LibraryManager.library, {
  mofei_wasm_ipc_send__sig: "viiii",
  mofei_wasm_ipc_send(channel, flags, payloadPtr, payloadLen) {
    if (typeof Module._mofeiWasmIpcSend === "function") {
      Module._mofeiWasmIpcSend(channel >>> 0, flags >>> 0, payloadPtr >>> 0, payloadLen >>> 0);
    }
  },
});
