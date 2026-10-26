import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import { startPosDevBridge } from "./posDevBridge";

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);

void startPosDevBridge().then((dispose) => {
  if (dispose) window.addEventListener("pagehide", dispose, { once: true });
}).catch((error) => console.error("[pos dev]", error));
