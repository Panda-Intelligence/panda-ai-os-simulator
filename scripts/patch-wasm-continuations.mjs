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

export function assertWasmPtySource(source) {
  const file = ts.createSourceFile("qemu-runtime.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  if (file.parseDiagnostics.length) fail("pty_javascript_syntax");
  const functions = [];
  function visit(node) {
    if (ts.isFunctionDeclaration(node) && node.name?.text === "xterm_pty_old_poll") functions.push(node);
    ts.forEachChild(node, visit);
  }
  visit(file);
  if (functions.length !== 1 || functions[0].parameters.map(node => node.name.getText(file)).join(",") !==
      "fds,nfds,timeout") fail("pty_poll_function");
  const calls = [], variables = new Map();
  function findPoll(node) {
    if (ts.isCallExpression(node) && /^(stream|ttyStream)\.stream_ops\.poll$/.test(compact(node.expression.getText(file)))) calls.push(node);
    ts.forEachChild(node, findPoll);
  }
  findPoll(functions[0]);
  function findVariables(node) {
    if (ts.isVariableDeclaration(node)) variables.set(node.name.getText(file), node.initializer);
    ts.forEachChild(node, findVariables);
  }
  findVariables(file);
  if (calls.length !== 2 || compact(calls[0].arguments[1]?.getText(file) ?? "") !== "stream.tty?0:-1"
      || compact(calls[1].getText(file)) !== "ttyStream.stream_ops.poll(ttyStream,timeout)") fail("pty_source_poll_scan");
  let readyBeforeWait = false;
  function findReady(node) {
    if (ts.isIfStatement(node) && compact(node.expression.getText(file)) === "nonzero||!ttyStream||!timeout"
        && /returnnonzero[;}]/.test(compact(node.thenStatement.getText(file)))) readyBeforeWait = true;
    ts.forEachChild(node, findReady);
  }
  findReady(functions[0]);
  if (!readyBeforeWait) fail("pty_source_ready_before_wait");
  const wait = variables.get("PTY_waitForReadableWithCallback");
  if (!wait || wait.parameters?.map(node => node.name.getText(file)).join(",") !== "callback,timeout") fail("pty_source_wait_arguments");
  const waitText = compact(wait.getText(file));
  if (waitText.indexOf("if(PTY.readable)") < 0 || waitText.indexOf("if(PTY.readable)") > waitText.indexOf("PTY.onReadable")) fail("pty_source_already_readable");
  const tty = variables.get("TTY");
  if (!tty || !compact(tty.getText(file)).includes("stream.flags&2048")) fail("pty_source_nonblocking_read");
  const selectFunctions = [];
  function findSelect(node) {
    if (ts.isFunctionDeclaration(node) && node.name?.text === "xterm_pty_old_newselect") selectFunctions.push(node);
    ts.forEachChild(node, findSelect);
  }
  findSelect(file);
  if (selectFunctions.length !== 1 || selectFunctions[0].parameters.map(node => node.name.getText(file)).join(",") !==
      "nfds,readfds,writefds,exceptfds,timeout") fail("pty_source_select_arguments");
  const selectText = compact(selectFunctions[0].getText(file));
  if (!selectText.includes("stream.tty?0:timeoutInMillis") || !selectText.includes("ttyStream.stream_ops.poll(ttyStream,timeoutInMillis)")) fail("pty_source_select_scan");
  const selectWrapper = variables.get("___syscall__newselect");
  if (!selectWrapper || !compact(selectWrapper.getText(file)).includes("PTY_selectTimeout(timeout)")) fail("pty_source_select_timeout_owner");
  return source;
}

export function patchWasmRootMailbox(source) {
  const file = ts.createSourceFile("qemu-runtime.js", source, ts.ScriptTarget.ESNext, true, ts.ScriptKind.JS);
  if (file.parseDiagnostics.length) fail("root_mailbox_javascript_syntax");
  const found = [];
  function visit(node) {
    if (ts.isVariableDeclaration(node) && node.name.getText(file) === "__emscripten_thread_mailbox_await") found.push(node);
    ts.forEachChild(node, visit);
  }
  visit(file);
  if (found.length !== 1 || !found[0].initializer || !ts.isArrowFunction(found[0].initializer)) fail("root_mailbox_function");
  const fn = found[0].initializer;
  if (fn.parameters.length !== 1 || fn.parameters[0].name.getText(file) !== "pthread_ptr" ||
      !ts.isBlock(fn.body) || fn.body.statements.length !== 1 || !ts.isIfStatement(fn.body.statements[0])) fail("root_mailbox_body");
  const condition = fn.body.statements[0].expression;
  const original = 'typeof Atomics.waitAsync === "function"';
  const replacement = 'ENVIRONMENT_IS_PTHREAD && typeof Atomics.waitAsync === "function"';
  if (compact(condition.getText(file)) === compact(replacement)) return source;
  if (compact(condition.getText(file)) !== compact(original)) fail("root_mailbox_condition");
  // 根运行时保留 SDK 的 postMessage 通知路径；PThread 继续使用原生异步等待。
  return source.slice(0, condition.getStart(file)) + replacement + source.slice(condition.end);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  if (process.argv[2] === "--check-pty" && process.argv.length === 4) {
    assertWasmPtySource(readFileSync(process.argv[3], "utf8"));
    process.exit(0);
  }
  const target = process.argv[2];
  if (!target || process.argv.length !== 3) throw new Error("Usage: node patch-wasm-continuations.mjs <runtime-js>");
  const source = readFileSync(target, "utf8");
  // PTY 行为来自编译前的依赖源补丁；这里只验证，禁止修改生成后的 PTY 函数。
  assertWasmPtySource(source);
  const patched = patchWasmRootMailbox(patchWasmContinuations(source));
  writeFileSync(target, patched);
  console.log("[patch-wasm-continuations] " + target);
}
