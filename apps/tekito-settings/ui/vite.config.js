import { defineConfig } from "vite";

// The dev server may read the repository (the license text for the mock host).
export default defineConfig({ base: "./", server: { fs: { allow: ["../../.."] } } });
