import assert from "node:assert/strict";
import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { createServer } from "vite";
import react from "@vitejs/plugin-react";
for (const key of ["PLAYWRIGHT_MODULE", "CHROME_EXECUTABLE", "SIMULATOR_TEST_OUT"]) {
  if (!process.env[key]) throw new Error(`Explicit ${key} required`);
}
const mod=await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const chromium=mod.chromium??mod.default?.chromium;
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
const out=resolve(process.env.SIMULATOR_TEST_OUT);await mkdir(out,{recursive:true});
const server=await createServer({root,configFile:false,plugins:[react(),{
  name:"location-privacy-fixture",configureServer(server){
    server.middlewares.use((req,res,next)=>{
      if (!req.url?.startsWith("/__location")) return next();
      res.setHeader("Content-Type","text/html");
      const html='<!doctype html><html><body><div id="root"></div><script type="module" src="/tests/fixtures/simulated-location.tsx"></script></body></html>';
      void server.transformIndexHtml(req.url,html).then(html=>res.end(html)).catch(next);
    });
  }
}],server:{host:"127.0.0.1",port:0,headers:{"Cross-Origin-Opener-Policy":"same-origin","Cross-Origin-Embedder-Policy":"require-corp"}}});
await server.listen();const origin=`http://127.0.0.1:${server.httpServer.address().port}`;
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const expected={name:"London",timezone:"Europe%2FLondon",latitude:51.5074,longitude:-0.1278};
const report={passed:false,scenarios:[],errors:[],external:[]};
try {
  for (const mode of ["denied","unavailable","granted"]) {
    for (const host of ["browser","desktop"]) {
      // Intentional non-London host environment; must never influence the preset.
      const context=await browser.newContext({viewport:{width:1440,height:1000},locale:"en-US",timezoneId:"Asia/Tokyo",serviceWorkers:"block"});
      try {
        await context.addInitScript(mode=>{
          const calls={geolocationAccess:0,position:0,permission:0};window.__privacyProbe=calls;
          Object.defineProperty(navigator,"geolocation",{configurable:true,get(){
            ++calls.geolocationAccess;
            if(mode==="unavailable")return undefined;
            return {
              getCurrentPosition(success,failure){++calls.position;
                if(mode==="granted")success({coords:{latitude:35.6762,longitude:139.6503}});
                else failure({code:1,message:"Test denied"});
              },
              watchPosition(){++calls.position;throw new Error("unexpected watch");},
            };
          }});
          if(navigator.permissions)navigator.permissions.query=async()=>{++calls.permission;return {state:mode};};
        },mode);
        const page=await context.newPage();page.on("pageerror",e=>report.errors.push(String(e)));
        await context.route("**/*",async route=>{
          const u=new URL(route.request().url());
          if(["http:","https:"].includes(u.protocol)&&u.origin!==origin){report.external.push(u.origin);await route.abort();}
          else await route.continue();
        });
        await page.goto(`${origin}/__location?host=${host}`);
        await page.getByText("test virtual SD",{exact:false}).first().waitFor();
        const boardSelect=page.locator('select[name="simulatorBoard"]');
        const boardIds=await boardSelect.locator("option").evaluateAll(options=>options.map(o=>o.value));
        for(const boardId of boardIds){
          await boardSelect.selectOption(boardId);
          if(host==="browser")await page.waitForFunction(id=>document.querySelector('select[name="simulatorFirmware"]')?.value.includes(id),boardId);
          await page.getByRole("button",{name:"Quick Launch",exact:true}).click();
          await page.getByRole("button",{name:"Stop",exact:true}).waitFor();
          let call=await page.evaluate(()=>window.__locationProbe.calls.filter(c=>c.location).at(-1));
          assert.deepEqual(call.location,expected,`${mode}/${host}/${boardId}/quick`);
          assert.equal(call.boardId,boardId);
          await page.getByRole("button",{name:"Reset",exact:true}).click();
          await page.waitForFunction(()=>window.__locationProbe.calls.filter(c=>c.location).at(-1)?.method==="reboot");
          call=await page.evaluate(()=>window.__locationProbe.calls.filter(c=>c.location).at(-1));
          assert.deepEqual(call.location,expected,`${mode}/${host}/${boardId}/reboot`);
          await page.getByRole("button",{name:"Stop",exact:true}).click();
          await page.getByRole("button",{name:"Quick Launch",exact:true}).waitFor();
          if(host==="desktop"){
            await page.locator(".ide-toolbar-firmware-button").click();
            await page.getByRole("button",{name:"Stop",exact:true}).waitFor();
            call=await page.evaluate(()=>window.__locationProbe.calls.filter(c=>c.location).at(-1));
            assert.deepEqual(call.location,expected,`${mode}/${boardId}/local`);
            assert.equal(call.firmwarePath,`/test/${boardId}.bin`);
            await page.getByRole("button",{name:"Stop",exact:true}).click();
          }
        }
        if(host==="desktop"){
          const before=await page.evaluate(()=>{window.__locationProbe.cancelPicker=true;return window.__locationProbe.calls.length;});
          await page.locator(".ide-toolbar-firmware-button").click();
          assert.equal(await page.evaluate(()=>window.__locationProbe.calls.length),before,"cancel must not start");
          await page.evaluate(()=>{window.__locationProbe.cancelPicker=false;window.__locationProbe.failNextStart=true;});
          await page.getByRole("button",{name:"Quick Launch",exact:true}).click();
          await page.getByText("Test start failure",{exact:true}).waitFor();
          await page.getByRole("button",{name:"Quick Launch",exact:true}).click();
          await page.getByRole("button",{name:"Stop",exact:true}).waitFor();
        }
        const privacy=await page.evaluate(()=>window.__privacyProbe);
        assert.deepEqual(privacy,{geolocationAccess:0,position:0,permission:0},`${mode}/${host} must not access geolocation`);
        const calls=await page.evaluate(()=>window.__locationProbe.calls.filter(c=>c.location));
        for(const call of calls)assert.deepEqual(call.location,expected);
        await page.locator('[data-simulated-location="London"]').waitFor();
        report.scenarios.push({mode,host,boards:boardIds.length,locationCalls:calls.length,privacy});
      } finally {await context.close();}
    }
  }
  assert.deepEqual(report.errors,[]);assert.deepEqual(report.external,[]);report.passed=true;
} catch(error){report.failure=String(error);throw error;}
finally {
  await writeFile(resolve(out,"result.json"),JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify(report));await browser.close();await server.close();
}
