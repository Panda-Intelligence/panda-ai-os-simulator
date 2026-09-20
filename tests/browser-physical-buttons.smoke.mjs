import assert from "node:assert/strict";
import { writeFile, mkdir } from "node:fs/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { createServer } from "vite";
import react from "@vitejs/plugin-react";
for (const key of ["PLAYWRIGHT_MODULE","CHROME_EXECUTABLE","SIMULATOR_TEST_OUT"]) {
  if (!process.env[key]) throw new Error(`Explicit ${key} required`);
}
const mod = await import(pathToFileURL(resolve(process.env.PLAYWRIGHT_MODULE)).href);
const chromium = mod.chromium ?? mod.default?.chromium;
const root=resolve(dirname(fileURLToPath(import.meta.url)),"..");
const out=resolve(process.env.SIMULATOR_TEST_OUT); await mkdir(out,{recursive:true});
const server = await createServer({root, configFile:false, plugins:[react(),{
  name:"isolated-physical-button-test", configureServer(server) {
    server.middlewares.use((req,res,next)=>{
      if (req.url !== "/__physical-buttons") return next();
      res.setHeader("Content-Type","text/html");
      void server.transformIndexHtml(req.url,'<!doctype html><html><body><div id="root"></div><script type="module" src="/tests/fixtures/physical-buttons.tsx"></script></body></html>').then(html=>res.end(html)).catch(next);
    });
  }
}],server:{host:"127.0.0.1",port:0,headers:{"Cross-Origin-Opener-Policy":"same-origin","Cross-Origin-Embedder-Policy":"require-corp"}}});
await server.listen();
const origin=`http://127.0.0.1:${server.httpServer.address().port}`;
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
const context=await browser.newContext({viewport:{width:1440,height:1000},serviceWorkers:"block"});
const page=await context.newPage(), errors=[],external=[];
page.on("pageerror",error=>errors.push(String(error)));
await context.route("**/*", async route=>{
  const u=new URL(route.request().url());
  if (["http:","https:"].includes(u.protocol) && u.origin!==origin) {external.push(u.origin);await route.abort();}
  else await route.continue();
});
// Independent expectations digitized from manufacturer drawing/photos, not CSS.
const expected={m5papers3:{PWR:{edge:"right",center:.825,id:0}},"lilygo-t5s3-pro":{
  RST:{edge:"right",center:.590,reset:true},PWR:{edge:"right",center:.680,id:6},
  BOOT:{edge:"left",center:.575,id:0},IO48:{edge:"left",center:.665,id:1}
}};
const report={passed:false,measurements:[],interactions:0,errors,external};
async function measure(boardId,scale) {
  return page.locator(".panel-physical-key").evaluateAll(nodes=>{
    const shell=document.querySelector(".panel-shell").getBoundingClientRect();
    return nodes.map(node=>{
      const cap=(node.querySelector(".panel-physical-key__cap")??node).getBoundingClientRect();
      const label=node.querySelector(".panel-physical-key__label").getBoundingClientRect();
      const centerX=(cap.left+cap.right)/2,centerY=(cap.top+cap.bottom)/2;
      return {label:node.getAttribute("aria-label"),center:(centerY-shell.top)/shell.height,
        edge:centerX<shell.left?"left":centerX>shell.right?"right":"inside",
        seam:Math.min(Math.abs(cap.left-shell.right),Math.abs(cap.right-shell.left)),
        labelDelta:Math.abs((label.top+label.bottom)/2-centerY),
        hit:document.elementFromPoint(centerX,centerY)?.closest("button")===node,
        overlapsPort:[...document.querySelectorAll(".panel-edge-port")].some(port=>{
          const p=port.getBoundingClientRect();return Math.min(cap.right,p.right)>Math.max(cap.left,p.left)+.1 && Math.min(cap.bottom,p.bottom)>Math.max(cap.top,p.top)+.1;
        }),width:cap.width,height:cap.height};
    });
  });
}
try {
  await page.goto(origin+"/__physical-buttons");
  await page.locator(".panel-physical-key").first().waitFor();
  for (const viewport of [{width:1440,height:1000},{width:1024,height:768},{width:800,height:640}]) {
    await page.setViewportSize(viewport);
    for (const [boardId, controls] of Object.entries(expected)) {
      await page.locator("[data-test-board]").selectOption(boardId);
      for (const scale of ["0","1","2"]) {
        await page.locator("[data-test-scale]").selectOption(scale);
        const values=await measure(boardId,scale);
        assert.equal(values.length,Object.keys(controls).length);
        for (const value of values) {
          const target=controls[value.label]; assert.ok(target,value.label);
          assert.equal(value.edge,target.edge,`${boardId} ${value.label} edge`);
          assert.ok(Math.abs(value.center-target.center)<.006,`${boardId} ${value.label} center ${value.center}, expected ${target.center}`);
          assert.ok(value.seam<2.1,`${boardId} ${value.label} floats off chassis by ${value.seam}px`);
          assert.ok(value.labelDelta<1,`${boardId} ${value.label} label drift ${value.labelDelta}px`);
          assert.equal(value.overlapsPort,false,`${boardId} ${value.label} overlaps port`);
          if(scale==="0") assert.equal(value.hit,true,`${boardId} ${value.label} is not hit-testable`);
        }
        report.measurements.push({boardId,scale,viewport,values});
      }
    }
  }
  await page.setViewportSize({width:1440,height:1000});
  await page.locator("[data-test-scale]").selectOption("0");
  for (const [boardId,controls] of Object.entries(expected)) {
    await page.locator("[data-test-board]").selectOption(boardId);
    for (const [name,spec] of Object.entries(controls)) {
      const key=page.locator(`.panel-physical-key[aria-label="${name}"]`);
      const pair=spec.reset?[{kind:"reset"}]:[{kind:"button",id:spec.id,pressed:true},{kind:"button",id:spec.id,pressed:false}];
      const clear=()=>page.evaluate(()=>window.__buttonProbe.events.splice(0));
      await clear(); await key.click();
      assert.deepEqual(await page.evaluate(()=>window.__buttonProbe.events),pair);
      for (const keyboard of ["Space","Enter"]) {
        await clear(); await key.focus(); await page.keyboard.press(keyboard);
        assert.deepEqual(await page.evaluate(()=>window.__buttonProbe.events),pair,`${boardId}/${name}/${keyboard}`);
        report.interactions++;
      }
      if (!spec.reset) {
        await clear(); const box=await key.boundingBox();
        await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.down();
        await page.evaluate(()=>window.dispatchEvent(new Event("blur"))); await page.mouse.up();
        assert.deepEqual(await page.evaluate(()=>window.__buttonProbe.events),pair,"blur must release exactly once");
      }
      report.interactions++;
    }
    // Decorative siblings or an iteration reorder must not move named controls.
    const before=await measure(boardId,"0");
    await page.evaluate(()=>{const rail=document.querySelector(".panel-physical-keys");window.__restoreKeys=[...rail.childNodes];rail.prepend(document.createElement("i"));rail.append(rail.querySelector("button"));});
    const after=await measure(boardId,"0");
    for (const value of before) assert.deepEqual(after.find(x=>x.label===value.label),value);
    await page.evaluate(()=>document.querySelector(".panel-physical-keys").replaceChildren(...window.__restoreKeys));
    await page.screenshot({path:resolve(out,`${boardId}.png`)});
  }
  await page.locator("[data-test-reset]").click();
  assert.ok(await page.locator('.panel-physical-key[aria-label="RST"]').isDisabled());
  await page.evaluate(()=>window.__buttonProbe.events.splice(0));
  const boot=page.locator('.panel-physical-key[aria-label="BOOT"]'); const box=await boot.boundingBox();
  await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.down();
  await page.evaluate(()=>document.querySelector("[data-test-mount]").click()); await page.mouse.up();
  assert.deepEqual(await page.evaluate(()=>window.__buttonProbe.events),[{kind:"button",id:0,pressed:true},{kind:"button",id:0,pressed:false}],"unmount releases held key");
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);report.passed=true;
} catch(error) {report.failure=String(error);throw error;}
finally {
  await writeFile(resolve(out,"result.json"),JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify({passed:report.passed,scenarios:report.measurements.length,interactions:report.interactions,failure:report.failure}));
  await context.close();await browser.close();await server.close();
}
