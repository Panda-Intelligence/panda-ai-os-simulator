import test, { before, after } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { spawnSync } from "node:child_process";

const source = readFileSync(new URL("../qemu-peripherals/target_xtensa_exc_helper.c", import.meta.url), "utf8");
const patch = readFileSync(new URL("../qemu-peripherals/xtensa-reset-vector.patch", import.meta.url), "utf8");
const signatures = [
  "void mofeiSimulatorTouchQueue(", "static void mofeiSimulatorButtonQueue(",
  "uint32_t HELPER(mofei_touch_read)(", "uint32_t HELPER(mofei_button_read)(",
];
function functionBody(text, signature) {
  const start = text.indexOf(signature);
  assert.ok(start >= 0, signature);
  let end = text.indexOf("{", start) + 1;
  for (let depth = 1; depth; end++) {
    if (text[end] === "{") depth++;
    if (text[end] === "}") depth--;
    assert.ok(end < text.length);
  }
  return text.slice(start, end);
}
function declarations(text) {
  const start = text.indexOf("typedef struct MofeiTouchSample {");
  const end = text.indexOf(signatures[0], start);
  assert.ok(start >= 0 && end > start);
  return text.slice(start, end);
}
function helperPostimage() {
  const start = patch.indexOf("diff --git a/target/xtensa/exc_helper.c ");
  const end = patch.indexOf("\ndiff --git ", start + 1);
  assert.ok(start >= 0 && end > start);
  let inHunk = false;
  return patch.slice(start, end).split("\n").flatMap(line => {
    if (line.startsWith("@@ ")) { inHunk = true; return []; }
    if (!inHunk) return [];
    if (line.startsWith("+") || line.startsWith(" ")) return [line.slice(1)];
    return line === "" ? [""] : [];
  }).join("\n");
}

const program = `
#include <glib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#define HELPER(name) helper_##name
typedef struct CPUXtensaState {
  uint32_t regs[64];
  uint8_t memory[16];
  unsigned callinc;
} CPUXtensaState;
${declarations(source)}
static bool check_unlocked_boundaries;
static void require_unlocked_boundary(void) {
  if (check_unlocked_boundaries) {
    assert(g_mutex_trylock(&g_input_queue_mutex));
    g_mutex_unlock(&g_input_queue_mutex);
  }
}
static void mofei_sim_board_touch_dimensions(uint16_t *width, uint16_t *height) {
  require_unlocked_boundary(); *width = 480; *height = 800;
}
static uint32_t mofei_skip_fn_common(CPUXtensaState *env, uint32_t *raw, unsigned *callinc) {
  require_unlocked_boundary(); *raw = 0x80000100; *callinc = env->callinc; return 0x40000100;
}
static void cpu_stw_data(CPUXtensaState *env, uint32_t ptr, uint16_t value) {
  require_unlocked_boundary(); assert(ptr + sizeof(value) <= sizeof(env->memory));
  memcpy(env->memory + ptr, &value, sizeof(value));
}
static void cpu_stb_data(CPUXtensaState *env, uint32_t ptr, uint8_t value) {
  require_unlocked_boundary(); assert(ptr < sizeof(env->memory)); env->memory[ptr] = value;
}
${signatures.map(signature => functionBody(source, signature)).join("\n")}
static bool touch_mode;
static pthread_mutex_t producer_bql = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool start_workers;
static atomic_uint consumed;
#define CONCURRENT_SAMPLES 2000u
#define MAX_OUTSTANDING 8u
static void push_sample(unsigned sequence) {
  pthread_mutex_lock(&producer_bql);
  if (touch_mode) {
    mofeiSimulatorTouchQueue(sequence % 400, (sequence * 7) % 700, sequence % 3 + 1, sequence % 2);
  } else {
    mofeiSimulatorButtonQueue(sequence % 7, (sequence / 7) % 2 != 0);
  }
  pthread_mutex_unlock(&producer_bql);
}
static CPUXtensaState pop_sample(void) {
  CPUXtensaState env = {.callinc = 2};
  for (unsigned i = 0; i < 64; i++) env.regs[i] = 0x11223344;
  memset(env.memory, 0xa5, sizeof(env.memory));
  uint32_t ret = touch_mode ? helper_mofei_touch_read(&env, 1, 3, 5, 6)
                            : helper_mofei_button_read(&env, 1, 2);
  assert(ret == 0x40000100);
  for (unsigned i = 0; i < 64; i++) if (i != 10) assert(env.regs[i] == 0x11223344);
  assert(env.regs[10] <= 1);
  return env;
}
static void check_sample(const CPUXtensaState *env, unsigned sequence) {
  assert(env->regs[10] == 1);
  if (touch_mode) {
    uint16_t x, y;
    memcpy(&x, env->memory + 1, sizeof(x)); memcpy(&y, env->memory + 3, sizeof(y));
    assert(x == sequence % 400); assert(y == (sequence * 7) % 700);
    assert(env->memory[5] == sequence % 3 + 1); assert(env->memory[6] == sequence % 2);
  } else {
    assert(env->memory[1] == sequence % 7); assert(env->memory[2] == (sequence / 7) % 2);
  }
}
static void expect_sample(unsigned sequence) {
  CPUXtensaState env = pop_sample(); check_sample(&env, sequence);
}
static void expect_empty(void) {
  CPUXtensaState env = pop_sample(); assert(env.regs[10] == 0);
  for (unsigned i = 0; i < sizeof(env.memory); i++) assert(env.memory[i] == 0xa5);
  assert((touch_mode ? g_touch_queue_count : g_button_queue_count) == 0);
}
static void single_thread_edges(void) {
  check_unlocked_boundaries = true;
  assert(MOFEI_TOUCH_QUEUE_CAP == 16 && MOFEI_BUTTON_QUEUE_CAP == 16);
  expect_empty();
  for (unsigned i = 0; i < 3; i++) push_sample(i);
  for (unsigned i = 0; i < 3; i++) expect_sample(i);
  expect_empty();
  for (unsigned i = 0; i < 16; i++) push_sample(i);
  assert((touch_mode ? g_touch_queue_count : g_button_queue_count) == 16);
  for (unsigned i = 0; i < 16; i++) expect_sample(i);
  expect_empty();
  for (unsigned i = 0; i < 19; i++) push_sample(i);
  assert((touch_mode ? g_touch_queue_count : g_button_queue_count) == 16);
  for (unsigned i = 3; i < 19; i++) expect_sample(i);
  expect_empty();
  for (unsigned round = 0; round < 32; round++) {
    unsigned base = round * 32;
    for (unsigned i = 0; i < 12; i++) push_sample(base + i);
    for (unsigned i = 0; i < 5; i++) expect_sample(base + i);
    for (unsigned i = 12; i < 26; i++) push_sample(base + i);
    assert((touch_mode ? g_touch_queue_count : g_button_queue_count) == 16);
    for (unsigned i = 10; i < 26; i++) expect_sample(base + i);
    expect_empty();
    assert((touch_mode ? g_touch_queue_head : g_button_queue_head) < 16);
  }
  if (touch_mode) {
    mofeiSimulatorTouchQueue(UINT16_MAX, UINT16_MAX, 3, 1);
    CPUXtensaState env = pop_sample(); uint16_t x, y;
    memcpy(&x, env.memory + 1, sizeof(x)); memcpy(&y, env.memory + 3, sizeof(y));
    assert(env.regs[10] == 1 && x == 479 && y == 799 && env.memory[5] == 3 && env.memory[6] == 1);
  } else {
    unsigned head = g_button_queue_head;
    mofeiSimulatorButtonQueue(255, true);
    assert(g_button_queue_head == head);
  }
  expect_empty();
  printf("%s FIFO/capacity16/drop-oldest/wrap/empty/ABI/unlocked-guest-boundaries PASS\\n", touch_mode ? "touch" : "button");
}
static void wait_start(void) {
  while (!atomic_load_explicit(&start_workers, memory_order_acquire)) sched_yield();
}
static void *producer(void *unused) {
  (void)unused; wait_start();
  for (unsigned i = 0; i < CONCURRENT_SAMPLES; i++) {
    while (i >= atomic_load_explicit(&consumed, memory_order_acquire) + MAX_OUTSTANDING) sched_yield();
    push_sample(i);
  }
  return NULL;
}
static void *consumer(void *unused) {
  (void)unused; wait_start();
  for (unsigned next = 0; next < CONCURRENT_SAMPLES;) {
    CPUXtensaState env = pop_sample();
    if (!env.regs[10]) {sched_yield(); continue;}
    check_sample(&env, next++);
    atomic_store_explicit(&consumed, next, memory_order_release);
  }
  return NULL;
}
static void concurrent_queue(void) {
  pthread_t p, c;
  assert(pthread_create(&p, NULL, producer, NULL) == 0);
  assert(pthread_create(&c, NULL, consumer, NULL) == 0);
  atomic_store_explicit(&start_workers, true, memory_order_release);
  assert(pthread_join(p, NULL) == 0); assert(pthread_join(c, NULL) == 0);
  assert(atomic_load(&consumed) == CONCURRENT_SAMPLES);
  expect_empty();
  printf("%s actual concurrent FIFO 2000/2000, max outstanding 8, no overflow PASS\\n", touch_mode ? "touch" : "button");
}
int main(int argc, char **argv) {
  assert(argc == 3); touch_mode = strcmp(argv[1], "touch") == 0;
  assert(touch_mode || strcmp(argv[1], "button") == 0);
  if (strcmp(argv[2], "edges") == 0) single_thread_edges();
  else {assert(strcmp(argv[2], "concurrent") == 0); concurrent_queue();}
  return 0;
}
`;

let dir, edgeBinary, tsanBinary;
before(() => {
  dir = mkdtempSync(join(tmpdir(), "panda-input-queue-"));
  const file = join(dir, "queue.c");
  writeFileSync(file, program);
  const pkg = spawnSync("pkg-config", ["--cflags", "--libs", "glib-2.0"], { encoding: "utf8" });
  assert.equal(pkg.status, 0, pkg.stderr);
  const flags = pkg.stdout.trim().split(/\s+/);
  for (const sanitizer of ["address,undefined", "thread"]) {
    const binary = join(dir, sanitizer === "thread" ? "queue-tsan" : "queue-edges");
    const built = spawnSync(process.env.CC || "cc", ["-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
      `-fsanitize=${sanitizer}`, "-fno-omit-frame-pointer", "-pthread", file, ...flags, "-o", binary], { encoding: "utf8" });
    assert.equal(built.status, 0, built.stderr);
    if (sanitizer === "thread") tsanBinary = binary; else edgeBinary = binary;
  }
});
after(() => { if (dir) rmSync(dir, { recursive: true, force: true }); });

test("input queue native patch and canonical C declarations/functions are identical", () => {
  const mirror = helperPostimage();
  assert.equal(declarations(mirror), declarations(source));
  for (const signature of signatures) assert.equal(functionBody(mirror, signature), functionBody(source, signature));
});

test("input queue critical sections contain only queue work and snapshot diagnostic depth", () => {
  for (const signature of signatures) {
    const body = functionBody(source, signature);
    const lock = body.indexOf("g_mutex_lock(&g_input_queue_mutex)");
    const unlock = body.indexOf("g_mutex_unlock(&g_input_queue_mutex)");
    assert.ok(lock >= 0 && unlock > lock, signature);
    assert.doesNotMatch(body.slice(lock, unlock), /cpu_st|fprintf|bql_|address_space|mofei_skip_fn_common/);
    assert.doesNotMatch(body.slice(0, lock) + body.slice(unlock), /g_(?:touch|button)_queue(?:\[|_head|_count)/);
    assert.match(body.slice(lock, unlock), /depth = g_(?:touch|button)_queue_count/);
  }
});

for (const queue of ["touch", "button"]) {
  test(`${queue} actual C preserves capacity16/drop-oldest/FIFO/wrap/empty and unlocks before guest access`, () => {
    const result = spawnSync(edgeBinary, [queue, "edges"], { encoding: "utf8", maxBuffer: 8 * 1024 * 1024, timeout: 15000 });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /FIFO\/capacity16\/drop-oldest\/wrap\/empty\/ABI\/unlocked-guest-boundaries PASS/);
  });
  test(`${queue} actual C producer/BQL and vCPU consumer stay FIFO and ThreadSanitizer clean`, () => {
    const result = spawnSync(tsanBinary, [queue, "concurrent"], {
      encoding: "utf8", maxBuffer: 8 * 1024 * 1024, timeout: 15000,
      env: { ...process.env, TSAN_OPTIONS: "halt_on_error=1:exitcode=66" },
    });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.doesNotMatch(result.stderr, /ThreadSanitizer|data race/);
    assert.match(result.stdout, /actual concurrent FIFO 2000\/2000, max outstanding 8, no overflow PASS/);
  });
}
