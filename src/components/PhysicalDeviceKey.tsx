import { useEffect, useRef, useState, type CSSProperties } from "react";
import type { PhysicalControl } from "../boardPhysicalControls";
import type { SimulatorHostBridge } from "../simulatorBridge";

type Props = {
  control: PhysicalControl;
  injectButton: SimulatorHostBridge["injectButton"];
  onReset?: () => void;
};

/** A calibrated, operable key. The caption and visible cap share one anchor. */
export function PhysicalDeviceKey({control,injectButton,onReset}: Props) {
  const held=useRef(new Set<string>());
  const [pressed,setPressed]=useState(false);
  const send=(down:boolean) => {
    if (control.buttonId !== undefined) void injectButton(control.buttonId,down).catch(()=>false);
  };
  const press=(source:string) => {
    if (held.current.has(source)) return;
    const first=held.current.size===0; held.current.add(source);
    if (first) {send(true);setPressed(true);}
  };
  const release=(source:string) => {
    if (!held.current.delete(source) || held.current.size) return;
    send(false);setPressed(false);
  };
  useEffect(()=>{
    const releaseAll=()=>{
      if (!held.current.size) return;
      held.current.clear();
      if (control.buttonId !== undefined) void injectButton(control.buttonId,false).catch(()=>false);
    };
    const blur=()=>{releaseAll();setPressed(false);};
    window.addEventListener("blur",blur);
    return ()=>{window.removeEventListener("blur",blur);releaseAll();};
  },[control.buttonId,injectButton]);

  const width=`${control.projection*100}%`;
  const style:CSSProperties={top:`${control.center*100}%`,height:`${control.length*100}%`,width,
    left:control.edge==="left"?`-${width}`:"auto",
    right:control.edge==="right"?`-${width}`:"auto"};
  return <button type="button" className={"panel-physical-key panel-physical-key--anchored"+(control.reset?" panel-physical-key--reset":"")}
    data-control={control.name} data-edge={control.edge} data-pressed={pressed || undefined}
    aria-label={control.name} title={control.name} style={style}
    disabled={Boolean(control.reset && !onReset)}
    onPointerDown={event=>{
      if (event.button!==0 || control.reset) return;
      event.preventDefault();event.currentTarget.focus({preventScroll:true});
      event.currentTarget.setPointerCapture(event.pointerId);press(`pointer:${event.pointerId}`);
    }}
    onPointerUp={event=>release(`pointer:${event.pointerId}`)}
    onPointerCancel={event=>release(`pointer:${event.pointerId}`)}
    onLostPointerCapture={event=>release(`pointer:${event.pointerId}`)}
    onKeyDown={event=>{
      if (event.key!==" " && event.key!=="Enter") return;
      event.stopPropagation(); // Do not also trigger the Studio global key shortcuts.
      if (control.reset) {if(event.repeat) event.preventDefault();return;}
      event.preventDefault();if(!event.repeat) press(`key:${event.key}`);
    }}
    onKeyUp={event=>{
      if (event.key!==" " && event.key!=="Enter") return;
      event.stopPropagation();
      if(control.reset) return;
      event.preventDefault();release(`key:${event.key}`);
    }}
    onBlur={()=>{for(const source of [...held.current]) if(source.startsWith("key:")) release(source);}}
    onClick={event=>{
      if(control.reset) {onReset?.();return;}
      // Assistive/programmatic activation has no pointer/keyboard hold sequence.
      if(event.detail===0 && held.current.size===0) {send(true);send(false);}
    }}>
    <span className="panel-physical-key__cap" aria-hidden="true"/>
    <span className="panel-physical-key__label" aria-hidden="true">{control.name}</span>
  </button>;
}
