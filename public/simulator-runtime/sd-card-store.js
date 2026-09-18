// Atomic chunked IndexedDB persistence. Legacy records are retained for rollback.
// One 128 MiB value exceeds Chromium's per-value serialization bound; each
// chunk is at most 4 MiB and metadata/chunks commit in one transaction.
const CHUNK_BYTES=4*1024*1024;
const MAX_BYTES=256*1024*1024;
const metaKey=key=>["panda-sd-meta-v2",key];
const chunkKey=(key,generation,index)=>["panda-sd-chunk-v2",key,generation,index];
function validMeta(value){
  return value && value.format==="chunks-v2" && typeof value.generation==="string" &&
    /^[a-z0-9-]{8,80}$/i.test(value.generation) && Number.isSafeInteger(value.byteLength) &&
    value.byteLength>0 && value.byteLength<=MAX_BYTES && value.chunkBytes===CHUNK_BYTES &&
    value.chunkCount===Math.ceil(value.byteLength/CHUNK_BYTES);
}
function legacyValue(value){
  if(!value)return null;
  if(!(value.bytes instanceof ArrayBuffer) || value.bytes.byteLength<1 || value.bytes.byteLength>MAX_BYTES ||
      value.byteLength!==value.bytes.byteLength)throw new Error("browser_sd_legacy_record_invalid");
  return {templateFingerprint:value.templateFingerprint,byteLength:value.byteLength,bytes:new Uint8Array(value.bytes)};
}
export async function readSdImage(openDb,storeName,key){
  const db=await openDb();
  return new Promise((resolve,reject)=>{
    const tx=db.transaction(storeName,"readonly"),store=tx.objectStore(storeName);
    let current,legacy,loaded=0,result=null,failure=null;
    const fail=error=>{failure=error;try{tx.abort();}catch{db.close();reject(error);}};
    tx.oncomplete=()=>{db.close();resolve(result);};
    tx.onerror=tx.onabort=()=>{db.close();reject(failure||tx.error||new Error("browser_sd_read_failed"));};
    const received=()=>{
      if(++loaded!==2)return;
      try{
        if(!current || (legacy && Number(legacy.savedAt)>Number(current.savedAt))){result=legacyValue(legacy);return;}
        if(!validMeta(current))throw new Error("browser_sd_chunk_metadata_invalid");
        const bytes=new Uint8Array(current.byteLength);let pending=current.chunkCount;
        for(let index=0;index<current.chunkCount;index++){
          const req=store.get(chunkKey(key,current.generation,index));
          req.onsuccess=()=>{
            const part=req.result,expected=Math.min(CHUNK_BYTES,current.byteLength-index*CHUNK_BYTES);
            if(!(part instanceof ArrayBuffer)||part.byteLength!==expected){fail(new Error("browser_sd_chunk_missing_or_invalid"));return;}
            bytes.set(new Uint8Array(part),index*CHUNK_BYTES);
            if(--pending===0)result={templateFingerprint:current.templateFingerprint,byteLength:current.byteLength,bytes};
          };
        }
      }catch(error){fail(error);}
    };
    const modern=store.get(metaKey(key));modern.onsuccess=()=>{current=modern.result;received();};
    const older=store.get(key);older.onsuccess=()=>{legacy=older.result;received();};
  });
}
export async function writeSdImage(openDb,storeName,key,value){
  if(!(value.bytes instanceof Uint8Array)||value.bytes.byteLength<1||value.bytes.byteLength>MAX_BYTES||
     value.byteLength!==value.bytes.byteLength)throw new Error("browser_sd_write_size_invalid");
  const generation=crypto.randomUUID();
  const chunks=[];
  for(let offset=0;offset<value.bytes.length;offset+=CHUNK_BYTES)chunks.push(value.bytes.slice(offset,offset+CHUNK_BYTES).buffer);
  const db=await openDb();
  return new Promise((resolve,reject)=>{
    const tx=db.transaction(storeName,"readwrite"),store=tx.objectStore(storeName);let failure=null;
    tx.oncomplete=()=>{db.close();resolve();};
    tx.onerror=tx.onabort=()=>{db.close();reject(failure||tx.error||new Error("browser_sd_write_aborted"));};
    const request=store.get(metaKey(key));
    request.onsuccess=()=>{
      try{
        const prior=request.result;
        if(prior && !validMeta(prior))throw new Error("browser_sd_chunk_metadata_invalid");
        for(let index=0;index<chunks.length;index++)store.put(chunks[index],chunkKey(key,generation,index));
        if(prior)for(let index=0;index<prior.chunkCount;index++)store.delete(chunkKey(key,prior.generation,index));
        store.put({format:"chunks-v2",generation,chunkBytes:CHUNK_BYTES,chunkCount:chunks.length,
          byteLength:value.byteLength,templateFingerprint:value.templateFingerprint,savedAt:value.savedAt??Date.now()},metaKey(key));
        // Deliberately preserve the legacy key. Older versions can still export
        // their last readable snapshot; migration never deletes it silently.
      }catch(error){failure=error;try{tx.abort();}catch{db.close();reject(error);}}
    };
  });
}
