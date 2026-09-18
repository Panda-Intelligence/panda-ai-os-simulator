// Local-only real guest smoke; not part of the asset-fixture unit test suite.
import assert from "node:assert/strict";
import { writeFile, mkdir } from "node:fs/promises";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";
const modulePath=process.env.PLAYWRIGHT_MODULE;
if (!modulePath || !process.env.SIMULATOR_TEST_URL || !process.env.SIMULATOR_TEST_OUT) throw new Error("Explicit test module, loopback URL and output directory required");
const { chromium }=await import(pathToFileURL(resolve(modulePath)).href);
const base=new URL(process.env.SIMULATOR_TEST_URL);
assert.ok(["127.0.0.1","localhost"].includes(base.hostname));
const out=resolve(process.env.SIMULATOR_TEST_OUT);await mkdir(out,{recursive:true});
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const context=await browser.newContext({viewport:{width:1280,height:1100},serviceWorkers:"block"});
const page=await context.newPage();const errors=[],blocked=[],failures=[];
let rejectFatal;
const fatal = new Promise((_, reject) => { rejectFatal=reject; });
void fatal.catch(()=>{});
page.on("pageerror",error=>{errors.push(String(error));rejectFatal(error);});
page.on("requestfailed",request=>failures.push({url:request.url(),error:request.failure()?.errorText}));
await context.route("**/*",async route=>{
  const url=new URL(route.request().url());
  if (["http:","https:"].includes(url.protocol) && url.origin!==base.origin) {
    blocked.push(url.origin);await route.abort();
  } else await route.continue();
});
let report={};
try {
  await page.goto(base.href,{waitUntil:"networkidle"});
  assert.equal(await page.evaluate(()=>window.crossOriginIsolated),true);
  await page.locator('select[name="simulatorBoard"]').selectOption(process.env.SIMULATOR_TEST_BOARD || "mofei");
  await page.getByRole("button",{name:/Quick Launch|快速啟動|快速启动/}).click();
  await Promise.race([page.waitForFunction(()=>document.querySelector(".pds-pill")?.textContent?.trim()==="start_sim: started",null,{timeout:180000}),fatal]);
  await Promise.race([page.waitForFunction(()=>{
    const canvas=document.querySelector("canvas.panel-canvas");
    if(!canvas)return false;
    const pixels=canvas.getContext("2d").getImageData(0,0,canvas.width,canvas.height).data;
    let dark=0,light=0;
    for(let i=0;i<pixels.length;i+=4){if(pixels[i]<64)dark++;if(pixels[i]>192)light++;}
    return dark>100 && light>100;
  },null,{timeout:180000}),fatal]);
  const canvas=page.locator("canvas.panel-canvas");
  const beforeInput=await canvas.evaluate(node=>node.toDataURL());
  await canvas.click({position:{x:100,y:100}});
  await Promise.race([page.waitForFunction(previous=>document.querySelector("canvas.panel-canvas").toDataURL()!==previous,beforeInput,{timeout:30000}),fatal]);
  report.inputChangedFrame=true;
  report={...report,...await page.evaluate(()=>{
    const canvas=document.querySelector("canvas.panel-canvas");
    const pixels=canvas.getContext("2d").getImageData(0,0,canvas.width,canvas.height).data;
    let dark=0,light=0;
    for(let i=0;i<pixels.length;i+=4){if(pixels[i]<64)dark++;if(pixels[i]>192)light++;}
    return {crossOriginIsolated:window.crossOriginIsolated,board:document.querySelector('select[name="simulatorBoard"]').value,
      status:document.querySelector(".pds-pill").textContent.trim(),canvas:{width:canvas.width,height:canvas.height,dark,light},
      visibleText:document.body.innerText.slice(-12000)};
  })};
  await page.screenshot({path:join(out,"guest.png"),fullPage:true});
  assert.equal(errors.length,0,errors.join("\n"));
  assert.ok(report.canvas.dark>100 && report.canvas.light>100);
  report.passed=true;
} catch(error) {
  // Allow the actual fatal-worker event's UI state update to settle before
  // collecting evidence; the runtime failure remains a failed test.
  await page.waitForTimeout(100).catch(()=>{});
  const failedSession=await page.evaluate(()=>({status:document.querySelector(".pds-pill")?.textContent,
    stopVisible:[...document.querySelectorAll("button")].some(button=>button.textContent?.trim()==="Stop")})).catch(()=>null);
  report={...report,passed:false,error:String(error),failedSession,visibleText:await page.locator("body").innerText().catch(()=>"")};
  await page.screenshot({path:join(out,"failure.png"),fullPage:true}).catch(()=>{});
  process.exitCode=1;
} finally {
  report={...report,errors,blockedExternalOrigins:[...new Set(blocked)],failedRequests:failures};
  await writeFile(join(out,"result.json"),JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify({...report,visibleText:report.visibleText?.slice(-1200)},null,2));
  await context.close();await browser.close();
}
