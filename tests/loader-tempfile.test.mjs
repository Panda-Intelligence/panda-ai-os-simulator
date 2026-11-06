import test, { before, after } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
// 仅用于冻结 patch 的负对照；生产加载器不读取这个测试变量。
const patchPath = process.env.LOADER_PATCH_TEST_SOURCE
  || join(root, "qemu-peripherals/esp32s3-soc-machine.patch");

function postimage(patch) {
  const lines = [];
  let inHunk = false;
  for (const line of patch.split("\n")) {
    if (line.startsWith("@@ ")) { inHunk = true; continue; }
    if (line.startsWith("--- ") || line.startsWith("+++ ")) { inHunk = false; continue; }
    if (!inHunk) continue;
    if (line.startsWith("+") || line.startsWith(" ")) lines.push(line.slice(1));
    else if (line === "") lines.push("");
  }
  return lines.join("\n");
}

function functionBody(source, signature) {
  const start = source.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = source.indexOf("{", start) + 1;
  for (let depth = 1; depth > 0; end++) {
    if (source[end] === "{") depth++;
    if (source[end] === "}") depth--;
    assert.ok(end < source.length);
  }
  return source.slice(start, end);
}

const canonicalSource = postimage(readFileSync(join(root, "qemu-peripherals/esp32s3-soc-machine.patch"), "utf8"));
const source = postimage(readFileSync(patchPath, "utf8"));
const writer = source.includes("static gchar* esp32s3_write_patched_elf(")
  ? functionBody(source, "static gchar* esp32s3_write_patched_elf(") : "";
function loaderBody(source) {
  const start = source.indexOf("      uint64_t manual_image_entry = 0;");
  const last = "ss->kernel_entry = image_entry;";
  const end = source.indexOf(last, start);
  assert.ok(start >= 0 && end > start);
  return source.slice(start, end + last.length);
}
// 原样执行 loader 的写入、错误退出、load_elf、手动 PT_LOAD 和清理段。
const loader = loaderBody(source);
const program = [
  "#include <glib.h>",
  "#include <glib/gstdio.h>",
  "#include <stdint.h>",
  "#include <stdbool.h>",
  "#include <stdio.h>",
  "#include <stdlib.h>",
  "#include <stdarg.h>",
  "#include <string.h>",
  "#include <signal.h>",
  "#include <sys/resource.h>",
  '#define ELFMAG "\\177ELF"',
  "#define EM_XTENSA 94",
  "#define MEMTXATTRS_UNSPECIFIED 0",
  "#define MEMTX_OK 0",
  "typedef int MemTxResult;",
  "typedef struct { int cpu[1]; uint64_t kernel_entry; } TestSoc;",
  "typedef struct { unsigned marker; gchar* filename; gboolean loaded, manual; } TestJob;",
  "static GPrivate active_job = G_PRIVATE_INIT(NULL);",
  "static GMutex barrier_mutex;",
  "static GCond barrier_cond;",
  "static unsigned arrived;",
  "static gboolean concurrent, fail_load, no_segments;",
  "static int address_space_memory;",
  "static struct { uint32_t murphyDeviceShellRunSimulatorLoopWithContext_addr; } mofei_sim_addrs;",
  "static struct { bool found; uint32_t addr, value; const char* name; } memset_patches[1];",
  "static void error_report(const char* format, ...) {",
  "  va_list args; va_start(args, format); vfprintf(stderr, format, args); va_end(args);",
  "  fputc(10, stderr); fflush(stderr);",
  "}",
  "static uint64_t translate_phys_addr(void* opaque, uint64_t address) { (void)opaque; return address; }",
  "static const char* load_elf_strerror(int code) { (void)code; return \"fixture load failure\"; }",
  "static gboolean payload_matches(const uint8_t* bytes, size_t len, unsigned marker) {",
  "  if (len != 64) return FALSE;",
  "  for (size_t i=0; i<len; i++) if (bytes[i] != (uint8_t)(marker+i)) return FALSE;",
  "  return TRUE;",
  "}",
  "static void write_u32(uint8_t* bytes, uint32_t value) { memcpy(bytes, &value, 4); }",
  "static gchar* make_elf(unsigned marker, gsize* len) {",
  "  *len=148; uint8_t* bytes=g_malloc0(*len); memcpy(bytes, ELFMAG, 4);",
  "  write_u32(bytes+24, 0x42000020u); write_u32(bytes+28, 52);",
  "  uint16_t count=no_segments?0:1; memcpy(bytes+44, &count, 2);",
  "  write_u32(bytes+52, 1); write_u32(bytes+56, 84);",
  "  write_u32(bytes+64, 0x42000020u); write_u32(bytes+68, 64);",
  "  for (unsigned i=0; i<64; i++) bytes[84+i]=(uint8_t)(marker+i);",
  "  return (gchar*)bytes;",
  "}",
  "static MemTxResult address_space_write(void* space, uint32_t address, int attrs, const void* bytes, size_t len) {",
  "  (void)space; (void)attrs; TestJob* job=g_private_get(&active_job);",
  "  if (address==0x42000020u) job->manual=payload_matches(bytes,len,job->marker);",
  "  return MEMTX_OK;",
  "}",
  "static MemTxResult address_space_read(void* space, uint32_t address, int attrs, void* bytes, size_t len) {",
  "  (void)space; (void)address; (void)attrs; memset(bytes,0,len); return MEMTX_OK;",
  "}",
  "static int load_elf(const char* filename, void* phdr, uint64_t (*translate)(void*,uint64_t), void* opaque,",
  "                    uint64_t* entry, uint64_t* low, void* high, void* flags, int be, int machine, int clear, int swab) {",
  "  (void)phdr; (void)translate; (void)opaque; (void)high; (void)flags;",
  "  (void)be; (void)machine; (void)clear; (void)swab;",
  "  fputs(\"TEST_LOAD_REACHED\",stderr); fflush(stderr);",
  "  TestJob* job=g_private_get(&active_job); job->filename=g_strdup(filename);",
  "  if (concurrent) {",
  "    g_mutex_lock(&barrier_mutex); arrived++;",
  "    if (arrived==8) g_cond_broadcast(&barrier_cond);",
  "    while (arrived<8) g_cond_wait(&barrier_cond,&barrier_mutex);",
  "    g_mutex_unlock(&barrier_mutex);",
  "  }",
  "  gchar* bytes=NULL; gsize len=0; GError* error=NULL;",
  "  if (!g_file_get_contents(filename,&bytes,&len,&error)) { g_clear_error(&error); return -1; }",
  "  job->loaded=len==148 && payload_matches((uint8_t*)bytes+84,len-84,job->marker);",
  "  g_free(bytes); *entry=0x42000020u; *low=*entry;",
  "  return fail_load?-1:(int)len;",
  "}",
  writer,
  "static int execute_loader(TestJob* job) {",
  "  g_private_set(&active_job,job); TestSoc soc={0}; TestSoc* ss=&soc;",
  "  const char* load_elf_filename=\"source-fixture.elf\";",
  "  gchar* elf_data; gsize elf_len=0; int image_size=-1;",
  "  uint64_t image_entry=0,image_lowaddr=0; elf_data=make_elf(job->marker,&elf_len);",
  loader,
  "  if (!job->loaded || !job->manual || ss->kernel_entry!=0x42000020u) {",
  "    fputs(\"TEST_PAYLOAD_MISMATCH\",stderr); return 2;",
  "  }",
  "  if (g_file_test(job->filename,G_FILE_TEST_EXISTS)) { fputs(\"TEST_TEMPFILE_LEAK\",stderr); return 3; }",
  "  return 0;",
  "}",
  "static gpointer run_job(gpointer opaque) { return GINT_TO_POINTER(execute_loader(opaque)); }",
  "int main(int argc, char** argv) {",
  "  if (argc!=2) return 90;",
  "  if (!strcmp(argv[1],\"write-failure\")) {",
  "    signal(SIGXFSZ,SIG_IGN); struct rlimit limit={0,0};",
  "    if (setrlimit(RLIMIT_FSIZE,&limit)!=0) return 91;",
  "  }",
  "  fail_load=!strcmp(argv[1],\"manual-fallback\") || !strcmp(argv[1],\"load-failure\");",
  "  no_segments=!strcmp(argv[1],\"load-failure\");",
  "  concurrent=!strcmp(argv[1],\"concurrent\");",
  "  if (!concurrent) { TestJob job={.marker=71}; int result=execute_loader(&job); g_free(job.filename); return result; }",
  "  TestJob jobs[8]={0}; GThread* threads[8]; int result=0;",
  "  g_mutex_init(&barrier_mutex); g_cond_init(&barrier_cond);",
  "  for (unsigned i=0;i<8;i++) { jobs[i].marker=17+i*19; threads[i]=g_thread_new(\"loader\",run_job,&jobs[i]); }",
  "  for (unsigned i=0;i<8;i++) { int code=GPOINTER_TO_INT(g_thread_join(threads[i])); if (code) result=code; }",
  "  for (unsigned i=0;i<8;i++) for(unsigned j=i+1;j<8;j++)",
  "    if (!g_strcmp0(jobs[i].filename,jobs[j].filename)) { fputs(\"TEST_DUPLICATE_PATH\",stderr); result=4; }",
  "  for (unsigned i=0;i<8;i++) g_free(jobs[i].filename);",
  "  g_mutex_clear(&barrier_mutex); g_cond_clear(&barrier_cond); return result;",
  "}",
].join("\n");

let fixture;
let binary;
before(() => {
  fixture = mkdtempSync(join(tmpdir(), "panda-loader-glib-"));
  const cFile = join(fixture, "loader.c");
  binary = join(fixture, "loader");
  writeFileSync(cFile, program);
  const config = spawnSync("pkg-config", ["--cflags", "--libs", "glib-2.0"], { encoding: "utf8" });
  assert.equal(config.status, 0, config.stdout + config.stderr);
  const build = spawnSync(process.env.CC || "cc", [
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", cFile, "-o", binary,
    ...config.stdout.trim().split(/\s+/),
  ], { encoding: "utf8", timeout: 30000 });
  assert.equal(build.status, 0, build.stdout + build.stderr);
});
after(() => { if (fixture) rmSync(fixture, { recursive: true, force: true }); });

function run(mode, missingDirectory = false) {
  const work = join(fixture, mode);
  if (!missingDirectory) mkdirSync(work);
  const result = spawnSync(binary, [mode], {
    env: { ...process.env, TMPDIR: work, LC_ALL: "C" },
    encoding: "utf8", timeout: 10000,
  });
  return { ...result, files: missingDirectory ? [] : readdirSync(work) };
}

test("native patch and WASM source mirror carry identical writer and loader lifetimes", () => {
  const mirror = readFileSync(join(root, "qemu-peripherals/hw_xtensa_esp32s3.c"), "utf8");
  const signature = "static gchar* esp32s3_write_patched_elf(";
  assert.equal(functionBody(canonicalSource, signature), functionBody(mirror, signature));
  assert.equal(loaderBody(canonicalSource), loaderBody(mirror));
  for (const text of [canonicalSource, mirror]) assert.match(text, /#include <glib\/gstdio.h>/);
});

test("concurrent actual loader paths preserve distinct payloads through both ELF consumers and clean up", () => {
  const result = run("concurrent");
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.deepEqual(result.files, []);
});

test("actual manual PT_LOAD fallback still succeeds and removes its temporary ELF", () => {
  const result = run("manual-fallback");
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.match(result.stderr, /continuing with 1 manually loaded PT_LOAD segments/);
  assert.deepEqual(result.files, []);
});

test("actual GLib write failure reports GError before any ELF load and removes reserved files", () => {
  const result = run("write-failure");
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stderr, /could not write patched ELF.*File too large.*domain=g-file-error-quark code=/);
  assert.doesNotMatch(result.stderr, /TEST_LOAD_REACHED|ELF load result/);
  assert.deepEqual(result.files, []);
});

test("missing TMPDIR reports actual GLib creation error before any ELF load", () => {
  const result = run("missing-directory", true);
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stderr, /could not write patched ELF.*No such file or directory.*domain=g-file-error-quark code=/);
  assert.doesNotMatch(result.stderr, /TEST_LOAD_REACHED|ELF load result/);
});

test("failed ordinary and manual loads remove the temporary ELF before the existing fatal exit", () => {
  const result = run("load-failure");
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.match(result.stderr, /TEST_LOAD_REACHED/);
  assert.match(result.stderr, /could not load ELF file 'source-fixture.elf'/);
  assert.deepEqual(result.files, []);
});
