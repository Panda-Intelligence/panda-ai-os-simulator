// IndexedDB 只保存已知的 SD object store，旧记录保留用于回滚和迁移。
export const CHUNK_BYTES=4*1024*1024;
export const MAX_BYTES=256*1024*1024;
const metaKey=key=>["panda-sd-meta-v2",key];
const chunkKey=(key,generation,index)=>["panda-sd-chunk-v2",key,generation,index];

export function validateSdImageBytes(value,expectedByteLength){
  const bytes=asUint8Array(value);
  if(!bytes || bytes.byteLength<1 || bytes.byteLength>MAX_BYTES)throw new Error("browser_sd_image_size_invalid");
  if(expectedByteLength!==undefined && (!Number.isSafeInteger(expectedByteLength) || expectedByteLength!==bytes.byteLength)){
    throw new Error("browser_sd_image_size_mismatch");
  }
  if(bytes.byteLength<512)throw new Error("browser_sd_fat_boot_sector_missing");
  const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
  const bytesPerSector=view.getUint16(11,true);
  if(![512,1024,2048,4096].includes(bytesPerSector))throw new Error("browser_sd_fat_sector_size_invalid");
  if(bytes.byteLength%bytesPerSector!==0)throw new Error("browser_sd_fat_image_alignment_invalid");
  if(bytes[bytesPerSector-2]!==0x55 || bytes[bytesPerSector-1]!==0xaa)throw new Error("browser_sd_fat_signature_invalid");
  const sectorsPerCluster=bytes[13];
  if(!sectorsPerCluster || sectorsPerCluster>128 || (sectorsPerCluster&(sectorsPerCluster-1))!==0){
    throw new Error("browser_sd_fat_cluster_geometry_invalid");
  }
  const reservedSectors=view.getUint16(14,true);
  const fatCount=bytes[16];
  const rootEntryCount=view.getUint16(17,true);
  const totalSectors16=view.getUint16(19,true);
  const fatSectors16=view.getUint16(22,true);
  const totalSectors=totalSectors16 || view.getUint32(32,true);
  const fatSectors=fatSectors16 || view.getUint32(36,true);
  if(!reservedSectors || (fatCount!==1 && fatCount!==2) || !totalSectors || !fatSectors){
    throw new Error("browser_sd_fat_bpb_invalid");
  }
  const rootDirSectors=Math.ceil((rootEntryCount*32)/bytesPerSector);
  const reservedEnd=reservedSectors;
  const fatEnd=reservedEnd+fatCount*fatSectors;
  const dataStart=fatEnd+rootDirSectors;
  if(reservedEnd>totalSectors || fatEnd>totalSectors || dataStart>=totalSectors || totalSectors*bytesPerSector>bytes.byteLength){
    throw new Error("browser_sd_fat_regions_invalid");
  }
  const dataSectors=totalSectors-dataStart;
  const clusterCount=Math.floor(dataSectors/sectorsPerCluster);
  const fatType=clusterCount<4085?12:clusterCount<65525?16:32;
  const minimumFatBytes=fatType===12?Math.ceil(((clusterCount+2)*3)/2):(clusterCount+2)*(fatType===16?2:4);
  if(fatSectors*bytesPerSector<minimumFatBytes)throw new Error("browser_sd_fat_table_too_small");
  if(fatType===32){
    if(rootEntryCount!==0 || fatSectors16!==0)throw new Error("browser_sd_fat32_bpb_invalid");
    const rootCluster=view.getUint32(44,true);
    if(rootCluster<2 || rootCluster>clusterCount+1)throw new Error("browser_sd_fat32_root_invalid");
  }else if(rootEntryCount===0 || fatSectors16===0){
    throw new Error("browser_sd_fat16_bpb_invalid");
  }
  const fatOffset=reservedSectors*bytesPerSector;
  if(fatOffset+3>bytes.byteLength || bytes[fatOffset]!==bytes[21] || bytes[fatOffset+1]!==0xff || bytes[fatOffset+2]!==0xff){
    throw new Error("browser_sd_fat_table_invalid");
  }
  return {byteLength:bytes.byteLength,sectorSize:bytesPerSector,sectorsPerCluster,totalSectors,fatType};
}

function asUint8Array(value){
  if(value instanceof Uint8Array)return value;
  if(value instanceof ArrayBuffer)return new Uint8Array(value);
  if(ArrayBuffer.isView(value))return new Uint8Array(value.buffer,value.byteOffset,value.byteLength);
  return null;
}

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
  // 所有写入入口都必须先通过同一份 FAT/大小校验，避免未来调用方绕过导入校验写入坏镜像。
  validateSdImageBytes(value.bytes,value.byteLength);
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
        // 保留旧 key，让旧版本仍可导出最后可读快照；迁移不静默删除它。
      }catch(error){failure=error;try{tx.abort();}catch{db.close();reject(error);}}
    };
  });
}
