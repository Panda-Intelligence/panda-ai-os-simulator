// Actual guest/Worker lifecycle on one page: no mocked boot or framebuffer.
import assert from "node:assert/strict";
import {mkdir,writeFile} from "node:fs/promises";
import {resolve} from "node:path";
import {pathToFileURL} from "node:url";
for(const key of ["PLAYWRIGHT_MODULE","CHROME_EXECUTABLE","SIMULATOR_TEST_URL","SIMULATOR_TEST_OUT"])if(!process.env[key])throw new Error(`Explicit ${key} required`);
const {chromium}=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const base=new URL(process.env.SIMULATOR_TEST_URL);assert.ok(["127.0.0.1","localhost"].includes(base.hostname));
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const context=await browser.newContext({viewport:{width:1440,height:1000},serviceWorkers:"block"});
const page=await context.newPage(),errors=[],external=[],results=[];
let failure=null;
page.on("pageerror",e=>errors.push(String(e)));
await context.route("**/*",r=>{const u=new URL(r.request().url());if(["http:","https:"].includes(u.protocol)&&u.origin!==base.origin){external.push(u.href);return r.abort();}return r.continue();});
await page.addInitScript(()=>{
 window.__lifecycle={created:0,terminated:0,frames:0,geolocation:0,logs:[]};
 Object.defineProperty(navigator,"geolocation",{configurable:true,get(){window.__lifecycle.geolocation++;return undefined;}});
 const Native=window.Worker;
 window.Worker=class extends Native{constructor(...args){super(...args);window.__lifecycle.created++;this.addEventListener("message",({data})=>{if(data.type==="framebuffer")window.__lifecycle.frames++;if(data.type==="serialLog")window.__lifecycle.logs.push(data.payload);});}terminate(){window.__lifecycle.terminated++;return super.terminate();}};
});
try{
 await page.goto(base.href,{waitUntil:"networkidle"});
 assert.equal(await page.evaluate(()=>crossOriginIsolated),true);
 const sequence=(process.env.SIMULATOR_TEST_BOARDS||"m5papers3,lilygo-t5s3-pro,s37uc,m5papers3").split(",");
 for(const id of sequence){
  await page.locator('[name="simulatorBoard"]').selectOption(id);
  await page.waitForFunction(()=>document.querySelector('.pds-btn--primary')?.disabled===false,null,{timeout:15000});
  const before=await page.evaluate(()=>({...window.__lifecycle}));
  const firmware=await page.locator('[name="simulatorFirmware"]').inputValue();
  await page.getByRole("button",{name:/Quick Launch/}).click();
  await page.waitForFunction(n=>window.__lifecycle.frames>n,before.frames,{timeout:60000});
  await page.waitForFunction(()=>{
   const c=document.querySelector('canvas.panel-canvas'),p=c.getContext('2d').getImageData(0,0,c.width,c.height).data;
   let dark=0,light=0;for(let i=0;i<p.length;i+=4){dark+=p[i]<64;light+=p[i]>192;}return dark>100&&light>100;
  },null,{timeout:20000});
  assert.equal(await page.locator('[name="simulatorBoard"]').isDisabled(),true);
  assert.equal(await page.locator('[name="simulatorFirmware"]').isDisabled(),true);
  assert.equal(await page.getByRole('button',{name:'Import files',exact:true}).isDisabled(),true);
  const canvas=page.locator('canvas.panel-canvas');
  await page.waitForTimeout(350);
  const image=await canvas.evaluate(c=>c.toDataURL()),box=await canvas.boundingBox();
  await canvas.click({position:{x:box.width*.5,y:box.height*.20}});
  await page.waitForFunction(image=>document.querySelector('canvas.panel-canvas').toDataURL()!==image,image,{timeout:15000});
  await page.getByRole('button',{name:'Stop',exact:true}).click();
  await page.waitForFunction(()=>document.querySelector('.pds-pill')?.textContent==='stop_sim: stopped',null,{timeout:60000});
  const after=await page.evaluate(()=>({...window.__lifecycle}));
  assert.equal(after.terminated,before.terminated+1,'Stop must terminate the actual worker');
  assert.equal(after.geolocation,0);
  assert.ok(after.logs.some(l=>l.includes('simulated location: London')),'London not installed');
  assert.ok(after.logs.some(l=>l.includes('persisted browser SD image reason=stop bytes=134217728')),'Stop did not persist 128 MiB');
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  results.push({id,firmware,frames:after.frames-before.frames,workerTerminated:true,sdPersisted:true});
  console.log(JSON.stringify(results.at(-1)));
 }
}catch(error){failure=String(error);process.exitCode=1;console.error(failure);}
finally{
 await mkdir(process.env.SIMULATOR_TEST_OUT,{recursive:true});
 await writeFile(resolve(process.env.SIMULATOR_TEST_OUT,"result.json"),JSON.stringify({passed:failure===null,results,errors,external,failure},null,2));
 await context.close();await browser.close();
}
