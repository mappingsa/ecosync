import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import basicSsl from "@vitejs/plugin-basic-ssl";

// ArcGIS sign-in only accepts HTTPS redirect URLs, so the dev server runs on
// https://localhost:5173 with a self-signed certificate. The browser warns
// about it once; that is expected on your own PC.
export default defineConfig({
  plugins: [react(), basicSsl()],
  server: { port: 5173, strictPort: true, host: "localhost" },
});
