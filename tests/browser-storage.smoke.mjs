// Real Chromium IndexedDB limit/transaction tests; no emulator or user data.
import assert from "node:assert/strict";
import { writeFile } from "node:fs/promises";
import { pathToFileURL } from "node:url";
const {chromium}=await import(pathToFileURL(process.env.PLAYWRIGHT_MODULE).href);
const origin=new URL(process.env.SIMULATOR_TEST_URL);
assert.equal(origin.hostname,"127.0.0.1");
const browser=await chromium.launch({headless:true,executablePath:process.env.CHROME_EXECUTABLE});
try{
  const page=await browser.newPage();
  await page.goto(new URL("simulator-runtime/sd-card-store.js",origin).href);
  const report=await page.evaluate(async()=>{
    const {readSdImage,writeSdImage}=await import("/simulator-runtime/sd-card-store.js");
    const name="panda-sd-proof-"+crypto.randomUUID(),storeName="cards",key="generated-only";
    const open=()=>new Promise((resolve,reject)=>{
      const request=indexedDB.open(name,1);
      request.onupgradeneeded=()=>request.result.createObjectStore(storeName);
      request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error);
    });
    const ensure=(condition,message)=>{if(!condition)throw new Error(message);};
    const data=new Uint8Array(128*1024*1024);data[0]=17;data[data.length-1]=93;
    await writeSdImage(open,storeName,key,{byteLength:data.length,bytes:data,templateFingerprint:"original",savedAt:2});
    const first=await readSdImage(open,storeName,key);
    ensure(first.byteLength===data.length && first.bytes[0]===17 && first.bytes[data.length-1]===93,"128MiB roundtrip");
    const failingOpen=async()=>{
      const db=await open();let puts=0;
      return {close:()=>db.close(),transaction:(...args)=>{
        const tx=db.transaction(...args);
        return new Proxy(tx,{get(target,prop){
          if(prop==="objectStore")return name=>{
            const store=target.objectStore(name);
            return new Proxy(store,{get(s,p){if(p==="put")return (...a)=>{if(++puts===2)throw new Error("injected transaction failure");return s.put(...a);};const v=Reflect.get(s,p,s);return typeof v==="function"?v.bind(s):v;}});
          }; const value=Reflect.get(target,prop,target);return typeof value==="function"?value.bind(target):value;
        },set(target,prop,value){return Reflect.set(target,prop,value,target);}});
      }};
    };
    let aborted=false;
    try{await writeSdImage(failingOpen,storeName,key,{bytes:data,byteLength:data.length,templateFingerprint:"must-rollback",savedAt:3});}
    catch{aborted=true;}
    ensure(aborted,"injected write must fail");
    const retained=await readSdImage(open,storeName,key);
    ensure(retained.templateFingerprint==="original" && retained.bytes[0]===17,"old image survives abort");
    const db=await open();
    await new Promise((resolve,reject)=>{const tx=db.transaction(storeName,"readwrite");tx.objectStore(storeName).put({bytes:new Uint8Array([1,2,3]).buffer,byteLength:3,templateFingerprint:"legacy",savedAt:1},"legacy");tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);});
    db.close();
    const legacy=await readSdImage(open,storeName,"legacy");ensure(legacy.bytes[2]===3,"legacy import");
    await writeSdImage(open,storeName,"legacy",{bytes:new Uint8Array([4,5,6]),byteLength:3,templateFingerprint:"upgraded",savedAt:2});
    const migrated=await readSdImage(open,storeName,"legacy");ensure(migrated.bytes[0]===4,"new-format read");
    const db2=await open();
    const preserved=await new Promise((resolve,reject)=>{const tx=db2.transaction(storeName);const request=tx.objectStore(storeName).get("legacy");request.onsuccess=()=>resolve(request.result);request.onerror=()=>reject(request.error);});
    db2.close();ensure(new Uint8Array(preserved.bytes)[0]===1,"old readable snapshot retained");
    const db3=await open();
    await new Promise((resolve,reject)=>{const tx=db3.transaction(storeName,"readwrite"),store=tx.objectStore(storeName),req=store.get(["panda-sd-meta-v2",key]);req.onsuccess=()=>store.delete(["panda-sd-chunk-v2",key,req.result.generation,0]);tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);});
    db3.close();let corruptRejected=false;
    try{await readSdImage(open,storeName,key);}catch{corruptRejected=true;}
    ensure(corruptRejected,"missing chunk must not become an empty image");
    await new Promise((resolve,reject)=>{const request=indexedDB.deleteDatabase(name);request.onsuccess=resolve;request.onerror=()=>reject(request.error);});
    return {passed:true,bytes:data.length,chunkBytes:4*1024*1024,atomicAbortPreserved:true,legacyRead:true,legacySnapshotPreserved:true,corruptChunkRejected:true};
  });
  await writeFile(process.env.SIMULATOR_TEST_OUT,JSON.stringify(report,null,2)+"\n");
  console.log(JSON.stringify(report));
}finally{await browser.close();}
