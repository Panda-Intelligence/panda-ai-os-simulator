import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import { PanelCanvas } from "../../src/components/PanelCanvas";
import { getSimulatorBoard, SIMULATOR_BOARDS } from "../../src/boards";
import "../../src/panda-ide.css";
import "../../src/App.css";

// Isolated component test, not a guest emulator; no framebuffer is fabricated.
const events: Array<{ kind: string; id?: number; pressed?: boolean }> = [];
Object.assign(window, { __buttonProbe: { events } });
// Production Studio has window-level Enter/Space shortcuts. Focused physical
// keys must consume those activations, not inject a second command/touch.
for (const type of ["keydown","keyup"]) window.addEventListener(type,event=>{
  const key=event as KeyboardEvent;
  if ((key.key===" " || key.key==="Enter") && key.target instanceof Element && key.target.closest(".panel-physical-key"))
    events.push({kind:"unconsumed-global-shortcut"});
});
const bridge = {
  injectButton: async (id: number, pressed: boolean) => { events.push({kind:"button",id,pressed}); return true; },
  injectTouch: async () => true,
  subscribeFramebuffer: async () => () => {},
};
function Harness() {
  const [boardId,setBoardId] = useState("m5papers3");
  const [scale,setScale] = useState<0|1|2>(0);
  const [resetEnabled,setResetEnabled] = useState(true);
  const [mounted,setMounted] = useState(true);
  return <>
    <div style={{display:"flex",gap:12,padding:12}}>
      <select data-test-board value={boardId} onChange={e=>setBoardId(e.target.value)}>
        {SIMULATOR_BOARDS.map(b=><option key={b.id} value={b.id}>{b.id}</option>)}
      </select>
      <select data-test-scale value={scale} onChange={e=>setScale(Number(e.target.value) as 0|1|2)}>
        <option value="0">Fit</option><option value="1">1x</option><option value="2">2x</option>
      </select>
      <button data-test-reset onClick={()=>setResetEnabled(x=>!x)}>Toggle reset</button>
      <button data-test-mount onClick={()=>setMounted(x=>!x)}>Toggle panel</button>
    </div>
    <div data-test-stage style={{display:"flex",alignItems:"flex-start",justifyContent:scale?"flex-start":"center",width:"100%",height:620,padding:48,boxSizing:"border-box",overflow:"auto"}}>
      {mounted && <PanelCanvas ariaLabel="button geometry test" hostBridge={bridge} board={getSimulatorBoard(boardId)} displayScale={scale}
        onReset={resetEnabled?()=>events.push({kind:"reset"}):undefined}/>}</div>
  </>;
}
createRoot(document.getElementById("root")!).render(<Harness/>);
