import test from "node:test";
import assert from "node:assert/strict";
import {mkdtempSync, readFileSync, writeFileSync, rmSync} from "node:fs";
import {homedir, tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";

const root = new URL("../", import.meta.url);
const source = name => readFileSync(new URL(`qemu-peripherals/${name}`, root), "utf8");
function fn(source, name) {
  const match = new RegExp(`(?:static )?(?:uint32_t|int|TaskHandle_t|char \\*|StackType_t \\*) ${name}\\([^;{}]*?\\)\\s*[^{};]*\\{`).exec(source);
  assert.ok(match, name);
  const start = match.index, open = source.indexOf("{", start);
  let depth = 1, end = open + 1;
  for (; depth; end++) { if (source[end] === "{") depth++; if (source[end] === "}") depth--; }
  return source.slice(start, end);
}

test("issued fake mutex ownership runs real IDF pthread and Queue holder semantics", () => {
  const helper = source("target_xtensa_exc_helper.c");
  const state = helper.slice(helper.indexOf("/* Serialized simulator queue metadata;"), helper.indexOf("static uint32_t mofei_murphy_or_app_main_entry"));
  const idf = process.env.IDF_PATH || join(homedir(), ".espressif/v6.0.1/esp-idf");
  const pthread = readFileSync(join(idf, "components/pthread/pthread.c"), "utf8");
  const queue = readFileSync(join(idf, "components/freertos/FreeRTOS-Kernel/queue.c"), "utf8");
  const tasks = readFileSync(join(idf, "components/freertos/FreeRTOS-Kernel/tasks.c"), "utf8");
  const additions = readFileSync(join(idf, "components/freertos/esp_additions/freertos_tasks_c_additions.h"), "utf8");
  const storageSource = readFileSync(new URL("../panda-os/device/main/StorageEsp.h",root),"utf8");
  const constructor = /StorageFilesystemMutex\(\) \{([^]*?)\n  \}/.exec(storageSource)[1];
  const semphr = readFileSync(join(idf,"components/freertos/FreeRTOS-Kernel/include/freertos/semphr.h"),"utf8");
  const recursiveStaticMacro = semphr.split("\n").find(line=>line.includes("#define xSemaphoreCreateRecursiveMutexStatic("));
  assert.ok(recursiveStaticMacro);
  const stackBase = source("mofei-sim-addrs.h").match(/#define MOFEI_SIM_FAKE_APP_STACK_TOP (0x[0-9a-f]+u)/)[1];
  const stackBytes = source("mofei-sim-addrs.h").match(/#define MOFEI_SIM_FAKE_TASK_STACK_BYTES (0x[0-9a-f]+u)/)[1];
  const symbols = [...state.matchAll(/QUEUE_TARGET\((\w+)\)/g)].map(m => m[1]).filter(n => n !== "name");
  const fields = [...new Set(symbols)];
  const dir = mkdtempSync(join(tmpdir(), "panda-mutex-"));
  try {
    const code = `
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#undef NULL
#define NULL 0
#define HELPER(name) helper_##name
#define MOFEI_SIM_FAKE_APP_STACK_BASE (${stackBase}-${stackBytes})
typedef struct { uint32_t regs[16]; } CPUXtensaState;
static struct { ${fields.map(n => `uint32_t ${n}_addr;`).join("\n")} } mofei_sim_addrs;
${state}
typedef uintptr_t TaskHandle_t;
typedef void* QueueHandle_t;
typedef unsigned TickType_t;
typedef struct { void* pcHead; struct { struct { TaskHandle_t xMutexHolder; } xSemaphore; } u; int xQueueLock; } Queue_t;
#define uxQueueType pcHead
#define queueQUEUE_IS_MUTEX NULL
#define configASSERT assert
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
${fn(queue, "xQueueGetMutexHolder").replace("xQueueGetMutexHolder", "realQueueGetMutexHolder")}
static uint32_t api(uint32_t target, uint32_t handle, uint32_t a3, uint32_t a4, unsigned callinc) {
  CPUXtensaState env = {0}; unsigned base = callinc == 4 ? 0 : callinc * 4;
  env.regs[base + 2] = handle; env.regs[base + 3] = a3;
  if (base + 4 < 16) env.regs[base + 4] = a4;
  assert(helper_mofei_queue_intercept(&env, target, callinc)); return env.regs[base + 2];
}
typedef struct { uint8_t bytes[96]; } StaticSemaphore_t;
#define queueQUEUE_TYPE_RECURSIVE_MUTEX MOFEI_QUEUE_RECURSIVE_MUTEX
static unsigned static_creates;
static uint32_t static_create(uint32_t kind, const StaticSemaphore_t* buffer) {
  assert(buffer); ++static_creates;
  return api(mofei_sim_addrs.xQueueCreateMutexStatic_addr,kind,0x3fcc1234,0,2);
}
#define xQueueCreateMutexStatic(kind,buffer) ((QueueHandle_t)(uintptr_t)static_create(kind,buffer))
${recursiveStaticMacro}
#define ESP_PLATFORM 1
static QueueHandle_t realStorageFilesystemMutexConstructor(void) {
  StaticSemaphore_t storage_; memset(&storage_,0x5a,sizeof(storage_));
  StaticSemaphore_t before=storage_; QueueHandle_t handle_=NULL;
  ${constructor}
  assert(!memcmp(&storage_,&before,sizeof(storage_))); return handle_;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return api(mofei_sim_addrs.xTaskGetCurrentTaskHandle_addr,0,0,0,4); }
static TaskHandle_t xSemaphoreGetMutexHolder(QueueHandle_t handle) {
  CPUXtensaState env = {0}; env.regs[2]=(uint32_t)(uintptr_t)handle;
  if (helper_mofei_queue_intercept(&env,mofei_sim_addrs.xQueueGetMutexHolder_addr,4)) return env.regs[2];
  return realQueueGetMutexHolder(handle);
}
static int xSemaphoreTake(QueueHandle_t h, TickType_t t) { return api(mofei_sim_addrs.xQueueSemaphoreTake_addr,(uintptr_t)h,t,0,4); }
static int xSemaphoreTakeRecursive(QueueHandle_t h, TickType_t t) { return api(mofei_sim_addrs.xQueueTakeMutexRecursive_addr,(uintptr_t)h,t,0,4); }
static int xSemaphoreGive(QueueHandle_t h) { return api(mofei_sim_addrs.xQueueGenericSend_addr,(uintptr_t)h,0,0,4); }
static int xSemaphoreGiveRecursive(QueueHandle_t h) { return api(mofei_sim_addrs.xQueueGiveMutexRecursive_addr,(uintptr_t)h,0,0,4); }
#define pdTRUE 1
#define PTHREAD_MUTEX_NORMAL 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2
typedef uintptr_t FixtureMutex;
typedef struct { QueueHandle_t sem; int type; } esp_pthread_mutex_t;
${fn(pthread,"pthread_mutex_lock_internal")}
${fn(pthread,"pthread_mutex_unlock").replace(/\bpthread_mutex_t\b/g, "FixtureMutex").replace("pthread_mutex_unlock", "fixture_pthread_mutex_unlock")}
typedef uint8_t StackType_t;
typedef struct { StackType_t* pxStack; char pcTaskName[16]; } TCB_t;
static unsigned real_task_reads;
static TCB_t* fixtureTCB(TaskHandle_t task) { ++real_task_reads; assert(task); return (TCB_t*)task; }
#define prvGetTCBFromHandle fixtureTCB
${fn(tasks,"pcTaskGetName").replace("pcTaskGetName", "realTaskGetName")}
${fn(additions,"xTaskGetStackStart").replace("xTaskGetStackStart", "realTaskGetStackStart")}
static uintptr_t taskQuery(uint32_t target, TaskHandle_t task) {
  CPUXtensaState env={0}; env.regs[2]=(uint32_t)task;
  if(helper_mofei_queue_intercept(&env,target,4)) return env.regs[2];
  return target==mofei_sim_addrs.xTaskGetStackStart_addr ? (uintptr_t)realTaskGetStackStart(task) : (uintptr_t)realTaskGetName(task);
}
int main(void) {
  ${fields.map((n,i) => `mofei_sim_addrs.${n}_addr=0x42001000u+${i*16}u;`).join("\n")}
  const uint32_t ownerA=mofei_serialized_task_token, ownerB=ownerA+4;
  assert(ownerA && xTaskGetCurrentTaskHandle()==ownerA);
  QueueHandle_t storageMutex=realStorageFilesystemMutexConstructor();
  assert(static_creates==1 && (uintptr_t)storageMutex!=4 && mofei_fake_queue((uintptr_t)storageMutex)->kind==4);
  assert(xSemaphoreTakeRecursive(storageMutex,0)==1); assert(xSemaphoreTakeRecursive(storageMutex,0)==1);
  assert(mofei_fake_queue((uintptr_t)storageMutex)->depth==2);
  assert(xSemaphoreGiveRecursive(storageMutex)==1 && mofei_fake_queue((uintptr_t)storageMutex)->depth==1);
  assert(xSemaphoreGiveRecursive(storageMutex)==1 && xSemaphoreGetMutexHolder(storageMutex)==0);
  for(unsigned c=1;c<=4;c++) {
    for(uint32_t kind=1;kind<=4;kind+=3) {
      CPUXtensaState create={0}; unsigned base=c==4?0:c*4;
      create.regs[base+2]=kind;create.regs[base+3]=0x3fcc1234;
      assert(helper_mofei_queue_intercept(&create,mofei_sim_addrs.xQueueCreateMutexStatic_addr|(c==2?0x80000000u:0),c));
      uint32_t handle=create.regs[base+2];
      assert(handle!=kind && mofei_fake_queue(handle)->kind==kind);
      assert(create.regs[base+3]==0x3fcc1234);
    }
  }
  for(unsigned c=1;c<=4;c++) for(unsigned explicitToken=0;explicitToken<2;explicitToken++) {
    uint32_t token=explicitToken?ownerA:0;
    assert(api(mofei_sim_addrs.xTaskGetStackStart_addr,token,0,0,c)==MOFEI_SIM_FAKE_APP_STACK_BASE);
    assert(api(mofei_sim_addrs.pcTaskGetName_addr,token,0,0,c)==0);
  }
  assert(real_task_reads==0);
  uintptr_t actualSP=${stackBase}-256;
  assert(actualSP-taskQuery(mofei_sim_addrs.xTaskGetStackStart_addr,0)==${stackBytes}-256);
  char diagnosticName[16]="unknown";
  const char* name=(const char*)taskQuery(mofei_sim_addrs.pcTaskGetName_addr,0);
  if(name) { strncpy(diagnosticName,name,sizeof(diagnosticName)-1); }
  assert(!strcmp(diagnosticName,"unknown"));
  StackType_t stack[64]; TCB_t realTask={.pxStack=stack,.pcTaskName="real-task"};
  assert(taskQuery(mofei_sim_addrs.xTaskGetStackStart_addr,(uintptr_t)&realTask)==(uintptr_t)stack);
  assert(!strcmp((char*)taskQuery(mofei_sim_addrs.pcTaskGetName_addr,(uintptr_t)&realTask),"real-task"));
  assert(real_task_reads==2);
  for (unsigned callinc=1;callinc<=4;callinc++) {
    CPUXtensaState e; for (unsigned i=0;i<16;i++) e.regs[i]=0xa000+i;
    CPUXtensaState before=e; unsigned base=callinc==4?0:callinc*4;
    assert(helper_mofei_queue_intercept(&e,mofei_sim_addrs.xTaskGetCurrentTaskHandle_addr|0x80000000u,callinc));
    assert(e.regs[base+2]==ownerA);
    for(unsigned i=0;i<16;i++) if(i!=base+2) assert(e.regs[i]==before.regs[i]);
  }
  uint32_t normal=api(mofei_sim_addrs.xQueueGenericCreate_addr,1,0,MOFEI_QUEUE_MUTEX,2);
  uint32_t recursive=api(mofei_sim_addrs.xQueueCreateMutex_addr,MOFEI_QUEUE_RECURSIVE_MUTEX,0,0,4);
  uint32_t caps=api(mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr,1,1,MOFEI_QUEUE_MUTEX,1);
  uint32_t base=api(mofei_sim_addrs.xQueueCreateWithCaps_addr,1,0,MOFEI_QUEUE_RECURSIVE_MUTEX,2);
  assert(mofei_fake_queue(normal)->kind==1 && mofei_fake_queue(caps)->kind==1);
  assert(mofei_fake_queue(recursive)->kind==4 && mofei_fake_queue(base)->kind==0);
  assert(api(mofei_sim_addrs.xQueueSemaphoreTake_addr,base,0,0,2)==1);
  assert(api(mofei_sim_addrs.xQueueGenericSend_addr,base,0,0,4)==1);
  assert(mofei_fake_queue(base)->depth==0 && mofei_fake_queue(base)->owner==0);
  for (unsigned i=0;i<2;i++) {
    uint32_t target=i?mofei_sim_addrs.xSemaphoreCreateGenericWithCaps_addr:mofei_sim_addrs.xQueueGenericCreate_addr;
    CPUXtensaState caller={0}; caller.regs[14]=1; caller.regs[15]=0;
    CPUXtensaState savedCaller=caller; uint32_t count=mofei_queue_count;
    assert(!helper_mofei_queue_intercept(&caller,target,3));
    assert(!memcmp(&caller,&savedCaller,sizeof(caller)) && mofei_queue_count==count);
    uint32_t entered=api(target,1,0,MOFEI_QUEUE_RECURSIVE_MUTEX,4);
    assert(mofei_fake_queue(entered)->kind==MOFEI_QUEUE_RECURSIVE_MUTEX);
  }
  assert(xSemaphoreGetMutexHolder((void*)(uintptr_t)normal)==0);
  assert(xSemaphoreGetMutexHolder((void*)(uintptr_t)base)==0);
  esp_pthread_mutex_t errorcheck={(void*)(uintptr_t)normal,PTHREAD_MUTEX_ERRORCHECK};
  FixtureMutex errorcheckRef=(uintptr_t)&errorcheck;
  assert(pthread_mutex_lock_internal(&errorcheck,0)==0);
  assert(xSemaphoreGetMutexHolder(errorcheck.sem)==ownerA);
  assert(pthread_mutex_lock_internal(&errorcheck,0)==EDEADLK);
  mofei_serialized_task_token=ownerB;
  assert(pthread_mutex_lock_internal(&errorcheck,0)==EBUSY);
  assert(xSemaphoreGive(errorcheck.sem)==0);
  assert(fixture_pthread_mutex_unlock(&errorcheckRef)==EPERM);
  assert(xSemaphoreGetMutexHolder(errorcheck.sem)==ownerA);
  mofei_serialized_task_token=ownerA; assert(fixture_pthread_mutex_unlock(&errorcheckRef)==0);
  assert(fixture_pthread_mutex_unlock(&errorcheckRef)==EPERM);
  esp_pthread_mutex_t recurse={(void*)(uintptr_t)recursive,PTHREAD_MUTEX_RECURSIVE};
  FixtureMutex recurseRef=(uintptr_t)&recurse;
  assert(pthread_mutex_lock_internal(&recurse,0)==0); assert(pthread_mutex_lock_internal(&recurse,0)==0);
  assert(mofei_fake_queue(recursive)->depth==2);
  mofei_serialized_task_token=ownerB; assert(fixture_pthread_mutex_unlock(&recurseRef)==EPERM);
  assert(xSemaphoreGiveRecursive(recurse.sem)==0);
  assert(mofei_fake_queue(recursive)->depth==2); mofei_serialized_task_token=ownerA;
  assert(fixture_pthread_mutex_unlock(&recurseRef)==0); assert(mofei_fake_queue(recursive)->depth==1);
  assert(xSemaphoreGetMutexHolder(recurse.sem)==ownerA);
  assert(fixture_pthread_mutex_unlock(&recurseRef)==0); assert(xSemaphoreGetMutexHolder(recurse.sem)==0);
  Queue_t real={.u.xSemaphore.xMutexHolder=ownerB};
  assert(xSemaphoreGetMutexHolder(&real)==ownerB); real.pcHead=(void*)1;
  assert(xSemaphoreGetMutexHolder(&real)==0);
  CPUXtensaState unknown={0}; unknown.regs[2]=MOFEI_FAKE_QUEUE_BASE+MOFEI_FAKE_QUEUE_STRIDE*100;
  CPUXtensaState saved=unknown;
  assert(!helper_mofei_queue_intercept(&unknown,mofei_sim_addrs.xQueueGetMutexHolder_addr,4));
  assert(!memcmp(&saved,&unknown,sizeof(saved)));
  assert(!mofei_fake_queue(normal+1)); assert(!mofei_fake_queue(MOFEI_FAKE_QUEUE_BASE-1));
  while(mofei_queue_count<MAX_QUEUE_ALLOCS) assert(mofei_next_queue_handle(0));
  assert(mofei_next_queue_handle(1)==0);
  puts("real IDF pthread + Queue holder, fake kind/owner/depth and register ABI PASS");
}
`;
    writeFileSync(join(dir,"mutex.c"),code);
    const sanitize = process.env.PANDA_MUTEX_SANITIZE === "1" ? ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] : [];
    const compiled=spawnSync("cc",["-std=c11","-Wall","-Wextra","-Werror",...sanitize,join(dir,"mutex.c"),"-o",join(dir,"mutex")],{encoding:"utf8"});
    assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
    const result=spawnSync(join(dir,"mutex"),[],{encoding:"utf8",timeout:5000});
    assert.equal(result.status,0,result.stdout+result.stderr);
    assert.match(result.stdout,/register ABI PASS/);
  } finally {rmSync(dir,{recursive:true,force:true});}
});

test("fake-only call and entry dispatch preserves real guest bodies",()=>{
  const translate=source("target_xtensa_translate.c"),machine=source("hw_xtensa_esp32s3.c");
  assert.match(translate,/mofei_try_queue_dispatch\(dc, tcg_constant_i32\(target\), par\[0\], false\)/);
  assert.match(translate,/mofei_try_queue_dispatch\(dc, arg\[0\]\.in, par\[0\], false\)/);
  assert.match(translate,/mofei_try_queue_dispatch\(dc, NULL, 0, true\)/);
  assert.match(translate,/TCG_COND_EQ, handled, 0, real_function/);
  for(const n of ["xQueueGetMutexHolder","xTaskGetCurrentTaskHandle","xQueueGenericSend","xQueueSemaphoreTake","xQueueTakeMutexRecursive","xQueueGiveMutexRecursive","xTaskGetStackStart","pcTaskGetName","xQueueCreateMutexStatic"]) {
    assert.ok(!machine.includes(`            "${n}",`),`${n} must retain its real body`);
    assert.ok(machine.includes(`{"${n}", &a->${n}_addr}`));
  }
});
