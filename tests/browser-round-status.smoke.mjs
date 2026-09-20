import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { createServer } from "vite";
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
for(const name of ["PLAYWRIGHT_MODULE","CHROME_EXECUTABLE","SIMULATOR_TEST_OUT"]) if(!process.env[name])throw new Error(`Explicit ${name} required`);
const { chromium }=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const server=await createServer({root,server:{host:"127.0.0.1",port:0}});await server.listen();
const base=`http://127.0.0.1:${server.httpServer.address().port}`;
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const context=await browser.newContext({serviceWorkers:"block"});const page=await context.newPage();const errors=[],external=[];
page.on("pageerror",e=>errors.push(String(e)));
await context.route("**/*",route=>{const u=new URL(route.request().url());if(["http:","https:"].includes(u.protocol)&&u.origin!==base){external.push(u.origin);return route.abort();}return route.continue();});
const report={passed:false,geometry:[],errors,external};
try {
 await page.goto(base,{waitUntil:"networkidle"});
 await page.waitForFunction(()=>document.querySelector(".pds-pill")?.textContent?.trim()==="NA");
 assert.match(await page.locator(".pds-pill").getAttribute("title"),/WASM|运行|執行/);
 assert.equal(await page.getByRole("button",{name:/Quick Launch/}).isDisabled(),true);
 for(const viewport of [{width:1440,height:1000},{width:1024,height:768},{width:800,height:640}]) {
  await page.setViewportSize(viewport);
  for(const id of ["m5papers3","lilygo-t5s3-pro"]) {
   await page.locator('[name="simulatorBoard"]').selectOption(id);
   for(const scale of ["fit","1x","2x"]) {
    await page.locator('[name="panelDisplayMode"]').selectOption(scale);
    const m=await page.evaluate(id=>{
     const shell=document.querySelector(".panel-shell"),target=document.querySelector(id==="m5papers3"?".panel-hanging-ear__ring":".panel-front-home-ring");
     const body=shell.getBoundingClientRect(),ring=target.getBoundingClientRect(),stage=document.querySelector(".ide-device-stage").getBoundingClientRect();
     const pill=document.querySelector(".pds-pill"),p=pill.getBoundingClientRect();
     return {width:ring.width,height:ring.height,offset:Math.abs((ring.left+ring.right-body.left-body.right)/2),bottom:ring.bottom,stageBottom:stage.bottom,buttonCount:shell.querySelectorAll("button").length,pill:{width:p.width,height:p.height,text:pill.textContent}};
    },id);
    assert.ok(Math.abs(m.width-m.height)<.25,`distorted ${id}/${scale}: ${JSON.stringify(m)}`);
    assert.ok(m.offset<1,`miscentered ${id}/${scale}`);
    if(scale==="fit")assert.ok(m.bottom< m.stageBottom,`clipped ring ${id}: ${JSON.stringify(m)}`);
    assert.equal(m.pill.text,"NA");assert.ok(m.pill.width<48&&m.pill.height<32);
    report.geometry.push({id,scale,viewport,...m});
   }
  }
 }
 assert.deepEqual(errors,[]);assert.deepEqual(external,[]);report.passed=true;
} finally {
 await mkdir(process.env.SIMULATOR_TEST_OUT,{recursive:true});
 await writeFile(resolve(process.env.SIMULATOR_TEST_OUT,"result.json"),JSON.stringify(report,null,2));
 console.log(JSON.stringify({passed:report.passed,scenarios:report.geometry.length,errors,external}));
 await context.close();await browser.close();await server.close();
}
