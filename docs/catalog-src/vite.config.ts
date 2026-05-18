import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import tailwindcss from '@tailwindcss/vite';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { resolve } from 'node:path';

export default defineConfig({
  plugins: [svelte(), tailwindcss(), viteSingleFile()],
  root: resolve(__dirname, 'src'),
  base: './',
  publicDir: resolve(__dirname, '../live-transmog'),
  build: {
    outDir: resolve(__dirname, '../live-transmog'),
    emptyOutDir: false,
    copyPublicDir: false,
    rollupOptions: {
      input: resolve(__dirname, 'src/catalog.html'),
    },
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
  },
  server: {
    fs: { allow: ['..', '../..'] },
  },
});
