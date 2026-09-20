import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
for(const name of ["PLAYWRIGHT_MODULE","CHROME_EXECUTABLE","SIMULATOR_TEST_URL","SIMULATOR_TEST_OUT"])if(!process.env[name])throw new Error(`Explicit ${name} required`);
const {chromium}=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const base=new URL(process.env.SIMULATOR_TEST_URL);assert.ok(["127.0.0.1","localhost"].includes(base.hostname));
const out=resolve(process.env.SIMULATOR_TEST_OUT);await mkdir(out,{recursive:true});
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const boards=(process.env.SIMULATOR_TEST_BOARDS||"mofei,s3r8,s3wroom,s37uc,m5papers3,lilygo-t5s3-pro").split(",");
const results=[];
try {for(const id of boards){
 const context=await browser.newContext({viewport:{width:1440,height:1000},serviceWorkers:"block"});
 const page=await context.newPage(),errors=[],external=[],requests=[];page.on("pageerror",e=>errors.push(String(e)));
 await context.route("**/*",r=>{const url=new URL(r.request().url());if(["http:","https:"].includes(url.protocol)&&url.origin!==base.origin){external.push(url.href);return r.abort();}if(/firmware|bootloader|symbols|qemu-system.*wasm/.test(url.pathname))requests.push(url.pathname);return r.continue();});
 await page.addInitScript(()=>{
  window.__guestProbe={frames:0,geometry:null,logs:[],runtimeErrors:[],geolocationAccess:0};
  Object.defineProperty(navigator,"geolocation",{configurable:true,get(){window.__guestProbe.geolocationAccess++;return undefined;}});
  const NativeWorker=window.Worker;
  window.Worker=class extends NativeWorker{constructor(...args){super(...args);this.addEventListener("message",({data:m})=>{
   if(m.type==="framebuffer"){window.__guestProbe.frames++;window.__guestProbe.geometry={width:m.payload.width,height:m.payload.height};}
   if(m.type==="serialLog"){window.__guestProbe.logs.push(m.payload);if(window.__guestProbe.logs.length>250)window.__guestProbe.logs.shift();}
   if(m.type==="simulatorError"){window.__guestProbe.runtimeErrors.push(m.payload);if(window.__guestProbe.runtimeErrors.length>100)window.__guestProbe.runtimeErrors.shift();}
  });}};
 });
 let report={id,passed:false};const started=Date.now();
 try{
  await page.goto(base.href,{waitUntil:"networkidle"});assert.equal(await page.evaluate(()=>crossOriginIsolated),true);
  await page.locator('[name="simulatorBoard"]').selectOption(id);
  await page.waitForFunction(()=>!document.querySelector('.pds-btn--primary')?.disabled,null,{timeout:15000});
  report.firmware=await page.locator('[name="simulatorFirmware"]').inputValue();
  await page.getByRole("button",{name:/Quick Launch/}).click();
  for(let second=0;second<60;second++){
   await page.waitForTimeout(1000);
   const probe=await page.evaluate(()=>({...window.__guestProbe,status:document.querySelector('.pds-pill')?.textContent}));
   if(second%10===0)console.log(JSON.stringify({id,second,frames:probe.frames,status:probe.status,tail:probe.logs.slice(-1)}));
   if(probe.frames>0){report.probe=probe;break;}
   if(errors.length||probe.runtimeErrors.some(e=>/Assertion failed|abort\(|RuntimeError|out of bounds|qemu_wasm_start_failed|browser_wasm_worker_fatal/.test(e))){report.probe=probe;break;}
  }
  report.probe=await page.evaluate(()=>({...window.__guestProbe,status:document.querySelector('.pds-pill')?.textContent}));
  assert.ok(report.probe.frames>0,'no actual worker framebuffer');
  assert.equal(report.probe.geolocationAccess,0,'must use simulated London, not host location');
  assert.equal(errors.length,0);assert.deepEqual(external,[]);
  assert.ok(!report.probe.runtimeErrors.some(e=>/Assertion failed|abort\(|out of bounds|qemu_wasm_start_failed/.test(e)),'runtime fatal error');
  await page.waitForFunction(()=>{
    const c=document.querySelector('canvas.panel-canvas'),p=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
    let dark=0,light=0;for(let i=0;i<p.length;i+=4){dark+=p[i]<64;light+=p[i]>192;}return dark>100&&light>100;
  },null,{timeout:15000});
  await page.waitForTimeout(350);
  const before=await page.evaluate(()=>window.__guestProbe.frames);
  const beforeImage=await page.locator('canvas.panel-canvas').evaluate(c=>c.toDataURL());
  const box=await page.locator('canvas.panel-canvas').boundingBox();
  await page.locator('canvas.panel-canvas').click({position:{x:box.width*.5,y:box.height*.20}});
  await page.waitForFunction(({n,image})=>window.__guestProbe.frames>n&&document.querySelector('canvas.panel-canvas').toDataURL()!==image,{n:before,image:beforeImage},{timeout:15000});
  const afterInput=await page.evaluate(()=>({...window.__guestProbe}));
  assert.equal(afterInput.geolocationAccess,0);
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  assert.ok(!afterInput.runtimeErrors.some(e=>/Assertion failed|abort\(|out of bounds|qemu_wasm_start_failed|browser_wasm_worker_fatal/.test(e)),"fatal after input");
  report.framesAfterInput=afterInput.frames;report.passed=true;
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
