import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  base: "/studio-simulator/",
  plugins: [react()],
  publicDir: false,
  build: { rollupOptions: { input: "studio.html" } },
});
