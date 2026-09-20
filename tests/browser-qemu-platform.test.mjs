import assert from "node:assert/strict";
import test from "node:test";
import {romClockLoaderArgs, createUc8253cStream, uc8253cRgbaToWire, installUc8253cDevice} from "../src/browserQemuPlatform.js";
function knownRom(){const rom=new Uint8Array(0x60000);for(const [offset,hex]of [[0x1a3c,"901a044091ffffa00900"],[0x41a90,"36410081f4ff28081df0"],[0x41a64,"58f7ce3f"]])rom.set(Buffer.from(hex,"hex"),offset);return rom;}
function frame(channel,payload,flags=0){const b=new Uint8Array(8+payload.length);b[0]=channel;b[1]=flags;new DataView(b.buffer).setUint32(4,payload.length,true);b.set(payload,8);return b;}
test("initialize reviewed ROM clock data without rewriting ROM or guest",()=>{const rom=knownRom(),before=rom.slice();assert.deepEqual(romClockLoaderArgs(rom),["-device","loader,addr=0x3fcef758,data=240,data-len=4"]);assert.deepEqual(rom,before);});
for(const where of [0x1a3c,0x41a90,0x41a64])test(`reject unreviewed ROM ABI at ${where}`,()=>{const rom=knownRom();rom[where]^=1;assert.throws(()=>romClockLoaderArgs(rom),/abi_mismatch/);});
test("reject unsupported ROM size or buffer type",()=>{assert.throws(()=>romClockLoaderArgs(new Uint8Array(5)),/abi_mismatch/);assert.throws(()=>romClockLoaderArgs(null),/abi_mismatch/);});
test("UC framebuffer arrives intact across split header boundaries",()=>{
 const image=Uint8Array.from({length:12480},(_,i)=>i%256),bytes=frame(1,image,1);
 for(const split of [1,2,3,4,5,6,7,8,9,100,12480]){const got=[];const parser=createUc8253cStream((...args)=>got.push(args));parser.push(bytes.subarray(0,split));parser.push(bytes.subarray(split));assert.equal(got.length,1);assert.deepEqual(got[0],[1,1,image]);}
});
test("UC coalesced frames/control/debug preserve raw bytes",()=>{const got=[],parser=createUc8253cStream((c,f,p)=>got.push([c,f,p]));const pix=new Uint8Array(12480).fill(255),ctl=new Uint8Array(10),dbg=new TextEncoder().encode("hello");parser.push(Uint8Array.from([...frame(1,pix),...frame(6,ctl),...frame(8,dbg)]));assert.deepEqual(got,[[1,0,pix],[6,0,ctl],[8,0,dbg]]);});
test("UC oversized frames poison the stream before allocation",()=>{for(const c of [0,1,6,8,255]){let events=0;const parser=createUc8253cStream(()=>events++),bad=new Uint8Array(8);bad[0]=c;new DataView(bad.buffer).setUint32(4,0xffffffff,true);assert.throws(()=>parser.push(bad),/invalid_header/);parser.push(frame(1,new Uint8Array(12480)));assert.equal(events,0);}});
test("UC malformed reserved bytes and framebuffer lengths are refused",()=>{for(const b of [frame(1,new Uint8Array(2)),frame(6,new Uint8Array(2))])assert.throws(()=>createUc8253cStream(()=>{}).push(b),/invalid_header/);const b=frame(1,new Uint8Array(12480));b[2]=1;assert.throws(()=>createUc8253cStream(()=>{}).push(b),/invalid_header/);});
function fsFixture(){let ops,node;class ErrnoError extends Error{constructor(errno){super(String(errno));this.errno=errno;}};return {ErrnoError,makedev:(a,b)=>(a<<8)|b,registerDevice(_dev,value){ops=value;},mkdev(path,mode,dev){node={path,mode,dev};},get ops(){return ops;},get node(){return node;}};}
test("UC isolated output device streams actual bytes without using serial stdout",()=>{
 const fs=fsFixture(),frames=[],path=installUc8253cDevice(fs,(...args)=>frames.push(args));
 assert.equal(path,"/dev/panda-uc8253c");assert.equal(fs.node.mode,0o600);
 const stream={seekable:true};fs.ops.open(stream);assert.equal(stream.seekable,false);assert.throws(()=>fs.ops.llseek(),e=>e.errno===70);
 const pixels=new Uint8Array(12480).fill(0xa5),bytes=frame(1,pixels);
 const heap8=new Int8Array(bytes.buffer);fs.ops.write({},heap8,0,5);fs.ops.write({},heap8,5,bytes.length-5);assert.deepEqual(frames,[[1,0,pixels]]);
});
test("UC portrait transform maps all four corners without inventing pixels",()=>{
 const portrait=new Uint8Array(240*416*4),pattern=[10,20,30,255];
 const points=[[0,0],[239,0],[0,415],[239,415],[103,299]];
 for(const [x,y] of points)portrait.set(pattern,(y*240+x)*4);
 const result=uc8253cRgbaToWire(portrait);
 for(const [x,y]of points)assert.deepEqual([...result.slice(((239-x)*416+y)*4,((239-x)*416+y)*4+4)],pattern);
 assert.equal(result.reduce((n,b)=>n+b,0),portrait.reduce((n,b)=>n+b,0));
});
test("UC rejects invalid output shape and missing device backend",()=>{
 assert.throws(()=>uc8253cRgbaToWire(new Uint8Array(1)),/size_invalid/);
 assert.throws(()=>installUc8253cDevice({},()=>{}),/unavailable/);
});
