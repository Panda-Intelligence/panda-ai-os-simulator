// ESP32-S3 revision-0 ROM ABI used by this simulator's direct-kernel boot.
// ROM startup/PLL setup is bypassed by the existing QEMU overlay. Restore its
// documented ticks-per-us DATA, not guest instructions, RNG output, or a frame.
export function romClockLoaderArgs(rom) {
  if (!(rom instanceof Uint8Array) || rom.length !== 0x60000) throw new Error("browser_rom_clock_abi_mismatch");
  const matches=(offset,hex)=>Array.from(hex.matchAll(/../g),m=>parseInt(m[0],16)).every((b,i)=>rom[offset+i]===b);
  if (!matches(0x1a3c,"901a044091ffffa00900") || !matches(0x41a90,"36410081f4ff28081df0") || !matches(0x41a64,"58f7ce3f")) throw new Error("browser_rom_clock_abi_mismatch");
  return ["-device", "loader,addr=0x3fcef758,data=240,data-len=4"];
}

// Existing UC8253C uses its native 8-byte chardev framing, not SSD1677's Wasm
// callback. Keep a distinct binary stream: never decode pixel bytes as text.
export function createUc8253cStream(onFrame) {
  let header=new Uint8Array(8), headerUsed=0, payload=null, used=0, poisoned=false;
  return {push(input) {
    if(poisoned) return;
    if(!(input instanceof Uint8Array)) throw new Error("browser_uc_stream_bytes_required");
    try {
      let offset=0;
      while(offset<input.length){
        if(headerUsed<8){const n=Math.min(8-headerUsed,input.length-offset);header.set(input.subarray(offset,offset+n),headerUsed);headerUsed+=n;offset+=n;if(headerUsed<8)break;
          const len=new DataView(header.buffer).getUint32(4,true), channel=header[0];
          const max=channel===1?12480:channel===6?64:channel===8?16384:0;
          if(header[2]||header[3]||!max||len>max||(channel===1&&len!==12480)||(channel===6&&len<10))throw new Error("browser_uc_stream_invalid_header");
          payload=new Uint8Array(len);used=0;
        }
        const n=Math.min(payload.length-used,input.length-offset);payload.set(input.subarray(offset,offset+n),used);used+=n;offset+=n;
        if(used===payload.length){const frame=payload;payload=null;headerUsed=0;onFrame(header[0],header[1],frame);}
      }
    } catch(error){poisoned=true;throw error;}
  }};
}
// The guest publishes a 240x416 portrait bitmap. Preserve the existing
// 416x240 host framebuffer ABI by rotating CCW; PanelCanvas rotates it back.
export function uc8253cRgbaToWire(portrait) {
  if (!(portrait instanceof Uint8Array) || portrait.length!==240*416*4) throw new Error("browser_uc_frame_size_invalid");
  const output=new Uint8Array(portrait.length);
  for(let y=0;y<416;y++)for(let x=0;x<240;x++){
    const src=(y*240+x)*4,dst=((239-x)*416+y)*4;
    output.set(portrait.subarray(src,src+4),dst);
  }
  return output;
}

// A dedicated write-only MEMFS character device keeps binary UC8253C frames
// out of mixed serial logs. Input still uses the existing exported guest queue.
export function installUc8253cDevice(fs,onFrame) {
  if (!fs || typeof fs.registerDevice!=="function" || typeof fs.mkdev!=="function")throw new Error("browser_uc_device_unavailable");
  const parser=createUc8253cStream(onFrame),dev=fs.makedev(240,0),path="/dev/panda-uc8253c";
  fs.registerDevice(dev,{
    open(stream){stream.seekable=false;},
    close(){},
    write(_stream,buffer,offset,length){parser.push(new Uint8Array(buffer.buffer,buffer.byteOffset+offset,length));return length;},
    llseek(){throw new fs.ErrnoError(70);},
  });
  fs.mkdev(path,0o600,dev);
  return path;
}
