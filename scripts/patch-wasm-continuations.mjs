import ts from "typescript";
import { readFileSync, writeFileSync } from "node:fs";
import { pathToFileURL } from "node:url";

const MARKER = "/* panda-asyncify-stack-ownership-v1 */";
const STATE = `${MARKER}
var pandaAsyncifyFramesV1 = { ffi: new Map(), invoke: new Map() };
function pandaTakeAsyncifyFrame(kind, name, identity) {
  var key = Asyncify.currData, frames = pandaAsyncifyFramesV1[kind].get(key);
  if (!frames || !frames.length) throw new Error("panda_asyncify_frame_missing:" + kind);
  var frame = frames.pop();
  if (!frames.length) pandaAsyncifyFramesV1[kind].delete(key);
  if (frame.name !== name || frame.identity.length !== identity.length ||
      frame.identity.some((value, index) => value !== identity[index])) {
    throw new Error("panda_asyncify_frame_mismatch:" + kind);
  }
  return frame;
}
function pandaSaveAsyncifyFrame(kind, frame) {
  var key = Asyncify.currData;
  if (!Number.isSafeInteger(key) || key <= 0) throw new Error("panda_asyncify_data_missing");
  var frames = pandaAsyncifyFramesV1[kind].get(key);
  if (!frames) { frames = []; pandaAsyncifyFramesV1[kind].set(key, frames); }
  if (frames.length >= 1024) throw new Error("panda_asyncify_frame_depth_exceeded");
  frames.push(frame);
}
`;
const fail = reason => { throw new Error(`qemu_asyncify_patch_shape_changed:${reason}`); };
const compact = value => value.replace(/\s+/g, "");

function patchFfi(node, file) {
  const parameters = node.parameters.map(parameter => parameter.name.getText(file));
  if (parameters.join(",") !== "cif,fn,rvalue,avalue") fail("ffi_parameters");
  const statements = [...node.body.statements];
  const index = statements.findIndex(statement => compact(statement.getText(file)) ===
    "varresult=getWasmTableEntry(fn).apply(null,args);");
  if (index < 1) fail("ffi_call");
  const prepare = statements.slice(0, index).map(statement => statement.getText(file)).join("\n");
  const finish = statements.slice(index + 1).map(statement => statement.getText(file)).join("\n");
  if (!compact(prepare).includes("varorig_stack_ptr=stackSave();") ||
      !compact(finish).startsWith("stackRestore(orig_stack_ptr);")) fail("ffi_stack");
  return `function ffi_call_js(cif, fn, rvalue, avalue) {
    var pandaFrame;
    if (Asyncify.state === 2) {
      pandaFrame = pandaTakeAsyncifyFrame("ffi", "ffi_call_js", [cif, fn, rvalue, avalue]);
      var orig_stack_ptr = pandaFrame.stack, args = pandaFrame.args;
      var ret_by_arg = pandaFrame.retByArg, rtype_id = pandaFrame.rtype;
    } else if (Asyncify.state === 0) { ${prepare} }
    else { throw new Error("panda_asyncify_invalid_ffi_entry"); }
    var result = getWasmTableEntry(fn).apply(null, args);
    if (Asyncify.state === 1) {
      pandaSaveAsyncifyFrame("ffi", pandaFrame || { name: "ffi_call_js",
        identity: [cif, fn, rvalue, avalue], stack: orig_stack_ptr,
        args: args, retByArg: ret_by_arg, rtype: rtype_id });
      return;
    }
    if (Asyncify.state !== 0) throw new Error("panda_asyncify_incomplete_ffi_rewind");
    ${finish}
  }`;
}
function patchInvoke(node, file) {
  const name = node.name.text;
  const parameters = node.parameters.map(parameter => parameter.name.getText(file));
  const statements = [...node.body.statements];
  if (parameters[0] !== "index" || statements.length !== 2 ||
      compact(statements[0].getText(file)) !== "varsp=stackSave();" ||
      !ts.isTryStatement(statements[1])) fail(name);
  const attempt = statements[1];
  if (!attempt.catchClause || attempt.finallyBlock || attempt.tryBlock.statements.length !== 1) fail(name + "_try");
  const callStatement = attempt.tryBlock.statements[0];
  const call = ts.isReturnStatement(callStatement) || ts.isExpressionStatement(callStatement)
    ? callStatement.expression?.getText(file) : null;
  if (!call || !compact(call).startsWith(name.replace("invoke_", "dynCall_") + "(")) fail(name + "_call");
  const catchPart = attempt.catchClause.getText(file);
  if (!compact(catchPart).includes("stackRestore(sp);")) fail(name + "_catch");
  return `function ${name}(${parameters.join(", ")}) {
    var pandaFrame, sp;
    if (Asyncify.state === 2) {
      pandaFrame = pandaTakeAsyncifyFrame("invoke", "${name}", [index]);
      sp = pandaFrame.stack;
    } else if (Asyncify.state === 0) { sp = stackSave(); }
    else { throw new Error("panda_asyncify_invalid_invoke_entry"); }
    try {
      var result = ${call};
      if (Asyncify.state === 1) pandaSaveAsyncifyFrame("invoke",
        pandaFrame || { name: "${name}", identity: [index], stack: sp });
      return result;
    } ${catchPart}
  }`;
}
// Generated libffi and invoke wrappers recreate JS locals during Asyncify
// rewind. Preserve the original stack snapshots per fiber's asyncify-data key.
// This retains marshalling and exception semantics; no RCU or stack guard changes.
export function patchWasmContinuations(source) {
  if (source.includes(MARKER)) {
    if (!source.includes(STATE) || !source.includes('pandaSaveAsyncifyFrame("ffi"') ||
        !source.includes('pandaTakeAsyncifyFrame("invoke"')) fail("invalid_existing_marker");
    return source;
  }
  const file = ts.createSourceFile("qemu-runtime.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  if (file.parseDiagnostics.length) fail("javascript_syntax");
  const ffi = [], invokes = [];
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name && node.body) {
      if (node.name.text === "ffi_call_js") ffi.push(node);
      else if (/^invoke_[vijfpd]+$/.test(node.name.text)) invokes.push(node);
    }
    ts.forEachChild(node, visit);
  }
  visit(file);
  if (ffi.length !== 1 || !invokes.length) fail("required_functions");
  if (invokes.some(node => node.parent !== ffi[0].parent)) fail("different_runtime_scopes");
  const edits = [{ start: ffi[0].getStart(file), end: ffi[0].end, text: STATE + patchFfi(ffi[0], file) },
    ...invokes.map(node => ({ start: node.getStart(file), end: node.end, text: patchInvoke(node, file) }))];
  for (const edit of edits.sort((a, b) => b.start - a.start)) {
    source = source.slice(0, edit.start) + edit.text + source.slice(edit.end);
  }
  return source;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const target = process.argv[2];
  if (!target || process.argv.length !== 3) throw new Error("Usage: node patch-wasm-continuations.mjs <runtime-js>");
  const source = readFileSync(target, "utf8");
  const patched = patchWasmContinuations(source);
  writeFileSync(target, patched);
  console.log("[patch-wasm-continuations] " + target);
}
