import assert from "node:assert/strict";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
for(const name of ["PLAYWRIGHT_MODULE","CHROME_EXECUTABLE","SIMULATOR_TEST_URL","SIMULATOR_TEST_OUT"])if(!process.env[name])throw new Error(`Explicit ${name} required`);
const {chromium}=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const base=new URL(process.env.SIMULATOR_TEST_URL);
// Read-only checks of a deployed simulator are opt-in; the default stays loopback only.
assert.ok(["127.0.0.1","localhost"].includes(base.hostname)||process.env.SIMULATOR_TEST_ALLOW_REMOTE==="1");
// A deployed page loads its own analytics beacons; tolerate only those when checking remotely.
const externalOrigins=list=>process.env.SIMULATOR_TEST_ALLOW_REMOTE==="1"?list.filter(u=>!/cloudflareinsights|google\.com\/ads|doubleclick|googletagmanager|google-analytics/.test(u)):list;
const registry=JSON.parse(await readFile(new URL("../boards.json",import.meta.url),"utf8"));
const boardById=Object.fromEntries(registry.boards.map(b=>[b.id,b]));
// Boards whose canvas pointer input was qualified before the key-only boards were added. They keep the touch gesture.
const TOUCH_QUALIFIED=new Set(["mofei","s3r8","s3wroom","s37uc","m5papers3","lilygo-t5s3-pro"]);
// Boot/response windows in ms. Boards without an entry use DEFAULT_WINDOWS.
// A measured entry is twice the slowest first-frame time measured on the 2.5.2 deployment (Chromium headless, host load 56-165),
// rounded up to 10 s; see docs/board-runtime-qualification.md, "Measured boot windows".
// Only boot is stretched: key/touch responses measured under 1 s (touch under 5 s), so ready/input stay at the default.
const DEFAULT_WINDOWS={boot:60000,ready:15000,input:15000};
const MEASURED_WINDOWS={
 "metalio-eink4":{boot:140000}, // first frame 58.4 s, 58.4 s, 67.4 s
 "molink":{boot:130000},        // first frame 62.8 s, 63.4 s
 "x4pro":{boot:140000},         // first frame 67.5 s, 59.7 s
};
// SIMULATOR_TEST_WINDOW_SCALE stretches every window on a loaded machine; it never shrinks one below the table.
const windowScale=Math.max(1,Number(process.env.SIMULATOR_TEST_WINDOW_SCALE??1)||1);
const windowsFor=id=>Object.fromEntries(Object.entries({...DEFAULT_WINDOWS,...(MEASURED_WINDOWS[id]??{})}).map(([k,v])=>[k,v*windowScale]));
// Navigation keys first: they move focus on every Home/Library screen. Power/select comes last because it can sleep the panel.
const KEY_ORDER=[/^(page ?)?down$/i,/^right$/i,/^(page ?)?up$/i,/^left$/i,/^(center|confirm|select|enter)$/i,/^back$/i,/^power$/i];
const keysFor=board=>[...board.keyMap].sort((a,b)=>{
 const rank=k=>{const i=KEY_ORDER.findIndex(re=>re.test(k.label));return i<0?KEY_ORDER.length:i;};return rank(a)-rank(b);
});
const out=resolve(process.env.SIMULATOR_TEST_OUT);await mkdir(out,{recursive:true});
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const boards=(process.env.SIMULATOR_TEST_BOARDS||"mofei,s3r8,s3wroom,s37uc,m5papers3,lilygo-t5s3-pro").split(",");
const results=[];
try {for(const id of boards){
 const board=boardById[id];assert.ok(board,`unknown board ${id}`);const win=windowsFor(id);
 const context=await browser.newContext({viewport:{width:1440,height:1000},serviceWorkers:"block"});
 const page=await context.newPage(),errors=[],external=[],requests=[];page.on("pageerror",e=>errors.push(String(e)));
 await context.route("**/*",r=>{const url=new URL(r.request().url());if(["http:","https:"].includes(url.protocol)&&url.origin!==base.origin){external.push(url.href);return r.abort();}if(/firmware|bootloader|symbols|qemu-system.*wasm/.test(url.pathname))requests.push(url.pathname);return r.continue();});
 await page.addInitScript(()=>{
  window.__guestProbe={frames:0,geometry:null,logs:[],runtimeErrors:[],geolocationAccess:0,serialTail:"",spiMounted:false,lastActivity:null};
  Object.defineProperty(navigator,"geolocation",{configurable:true,get(){window.__guestProbe.geolocationAccess++;return undefined;}});
  const NativeWorker=window.Worker;
  window.Worker=class extends NativeWorker{constructor(...args){super(...args);this.addEventListener("message",({data:m})=>{
   if(m.type==="serialLog"||m.type==="simulatorError"){
    window.__guestProbe.serialTail=(window.__guestProbe.serialTail+String(m.payload)).slice(-1024);
    if(window.__guestProbe.serialTail.includes("E2E:STORAGE:event=sdspi_mounted "))window.__guestProbe.spiMounted=true;
    const activity=[...window.__guestProbe.serialTail.matchAll(/UIREFRESH activity=([^ ]+) mode=/g)].at(-1);
    if(activity)window.__guestProbe.lastActivity=activity[1];
   }
   if(m.type==="framebuffer"){window.__guestProbe.frames++;window.__guestProbe.geometry={width:m.payload.width,height:m.payload.height};}
   if(m.type==="serialLog"){window.__guestProbe.logs.push(m.payload);if(window.__guestProbe.logs.length>250)window.__guestProbe.logs.shift();}
   if(m.type==="simulatorError"){window.__guestProbe.runtimeErrors.push(m.payload);if(window.__guestProbe.runtimeErrors.length>100)window.__guestProbe.runtimeErrors.shift();}
  });}};
 });
 let report={id,passed:false};const started=Date.now();
 try{
  await page.goto(base.href,{waitUntil:"networkidle"});assert.equal(await page.evaluate(()=>crossOriginIsolated),true);
  await page.locator('[name="simulatorBoard"]').selectOption(id);
  await page.waitForFunction(()=>!document.querySelector('.pds-btn--primary')?.disabled,null,{timeout:win.ready});
  report.firmware=await page.locator('[name="simulatorFirmware"]').inputValue();
  await page.getByRole("button",{name:/Quick Launch/}).click();
  const launched=Date.now();
  for(let second=0;second<win.boot/1000;second++){
   await page.waitForTimeout(1000);
   const probe=await page.evaluate(()=>({...window.__guestProbe,status:document.querySelector('.pds-pill')?.textContent}));
   if(second%10===0)console.log(JSON.stringify({id,second,frames:probe.frames,status:probe.status,tail:probe.logs.slice(-1)}));
   if(probe.frames>0){report.probe=probe;report.firstFrameMs=Date.now()-launched;break;}
   if(errors.length||probe.runtimeErrors.some(e=>/Assertion failed|abort\(|RuntimeError|out of bounds|qemu_wasm_start_failed|browser_wasm_worker_fatal/.test(e))){report.probe=probe;break;}
  }
  report.probe=await page.evaluate(()=>({...window.__guestProbe,status:document.querySelector('.pds-pill')?.textContent}));
  assert.ok(report.probe.frames>0,'no actual worker framebuffer');
  assert.equal(report.probe.geolocationAccess,0,'must use simulated London, not host location');
  assert.equal(errors.length,0);assert.deepEqual(externalOrigins(external),[]);
  assert.ok(!report.probe.runtimeErrors.some(e=>/Assertion failed|abort\(|out of bounds|qemu_wasm_start_failed/.test(e)),'runtime fatal error');
  await page.waitForFunction(()=>{
    const c=document.querySelector('canvas.panel-canvas'),p=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
    let dark=0,light=0;for(let i=0;i<p.length;i+=4){dark+=p[i]<64;light+=p[i]>192;}return dark>100&&light>100;
  },null,{timeout:win.ready});
  await page.waitForTimeout(350);
  report.paintMs=Date.now()-launched-report.firstFrameMs;
  if(["m5papers3","lilygo-t5s3-pro"].includes(id)){
   await page.waitForFunction(()=>window.__guestProbe.spiMounted===true,null,{timeout:win.ready});
   report.probe.spiMounted=true;
  }
  const canvas=page.locator('canvas.panel-canvas');
  const frameChanged=async(n,image,timeout)=>page.waitForFunction(({n,image})=>window.__guestProbe.frames>n&&document.querySelector('canvas.panel-canvas').toDataURL()!==image,{n,image},{timeout}).then(()=>true,()=>false);
  const snapshot=async()=>({n:await page.evaluate(()=>window.__guestProbe.frames),image:await canvas.evaluate(c=>c.toDataURL())});
  // Touch gesture on the boards qualified with it; informational on other touch-capable boards.
  const hasTouch=board.qemu.touchType!=="none"&&board.capabilities.includes("input.touch");
  report.input={touch:null,key:null,keysTried:[]};
  if(TOUCH_QUALIFIED.has(id)||hasTouch){
   const {n,image}=await snapshot();const box=await canvas.boundingBox();const started=Date.now();
   await canvas.click({position:{x:box.width*.5,y:box.height*.20}});
   const changed=await frameChanged(n,image,win.input);
   report.input.touch={changed,ms:Date.now()-started,required:TOUCH_QUALIFIED.has(id)};
   if(TOUCH_QUALIFIED.has(id))assert.ok(changed,"touch gesture did not change the framebuffer");
  }
  // Boards without qualified touch are driven with their own keys from boards.json keyMap.
  if(!TOUCH_QUALIFIED.has(id)){
   for(const key of keysFor(board)){
    const {n,image}=await snapshot();const button=page.locator(`.panel-physical-key[aria-label="${key.label}"]`);
    assert.equal(await button.count(),1,`no physical key control for ${id}/${key.label}`);
    const box=await button.boundingBox();const started=Date.now();
    await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.down();await page.waitForTimeout(250);await page.mouse.up();
    const changed=await frameChanged(n,image,win.input);
    report.input.keysTried.push({label:key.label,id:key.id,changed,ms:Date.now()-started});
    if(changed){report.input.key={label:key.label,id:key.id};break;}
   }
   assert.ok(report.input.key,`${id}: none of its keys changed the framebuffer: ${JSON.stringify(report.input.keysTried)}`);
  }
  const afterInput=await page.evaluate(()=>({...window.__guestProbe}));
  assert.equal(afterInput.geolocationAccess,0);
  assert.deepEqual(errors,[]);assert.deepEqual(externalOrigins(external),[]);
  assert.ok(!afterInput.runtimeErrors.some(e=>/Assertion failed|abort\(|out of bounds|qemu_wasm_start_failed|browser_wasm_worker_fatal/.test(e)),"fatal after input");
  report.framesAfterInput=afterInput.frames;
  if(process.env.SIMULATOR_TEST_READ_SAMPLE==="1"){
   // This opt-in journey requires the documented single-book local seed.
   await page.waitForFunction(()=>window.__guestProbe.lastActivity==="Library",null,{timeout:15000});
   const canvas=page.locator("canvas.panel-canvas");
   const library=await canvas.evaluate(c=>c.toDataURL());const box=await canvas.boundingBox();
   await canvas.click({position:{x:box.width*.5,y:box.height*.255}});
   await page.waitForFunction(()=>window.__guestProbe.lastActivity==="Reader",null,{timeout:30000});
   await page.waitForFunction(image=>document.querySelector("canvas.panel-canvas").toDataURL()!==image,library,{timeout:15000});
   report.readingFromSd=true;
  }
  assert.deepEqual(errors,[]);report.passed=true;
 }catch(e){report.error=String(e);process.exitCode=1;}
 finally{
  report.probe=await page.evaluate(()=>({...window.__guestProbe,status:document.querySelector('.pds-pill')?.textContent})).catch(()=>report.probe);
  report={...report,elapsedMs:Date.now()-started,errors,external,requests:[...new Set(requests)]};
  await writeFile(resolve(out,id+'.json'),JSON.stringify(report,null,2));
  await page.screenshot({path:resolve(out,id+'.png'),fullPage:true}).catch(()=>{});
  console.log(JSON.stringify({id,passed:report.passed,frames:report.probe?.frames,error:report.error,tail:report.probe?.logs.slice(-2),runtimeErrors:report.probe?.runtimeErrors.slice(-4)}));
  results.push({id,passed:report.passed,elapsedMs:report.elapsedMs});await context.close();
 }
}}finally{await browser.close();await writeFile(resolve(out,'summary.json'),JSON.stringify(results,null,2));}
