import { defineConfig, loadEnv } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '');
  const target = env.RATS_API_ORIGIN || 'http://127.0.0.1:18095';
  const wsTarget = env.RATS_WS_ORIGIN || 'ws://127.0.0.1:18096';
  return { plugins: [react()], server: { strictPort: true, proxy: {
    '/api': { target, changeOrigin: true }, '/readyz': { target, changeOrigin: true },
    '/ws': { target: wsTarget, ws: true, rewrite: path => path.replace(/^\/ws/, '/') }
  } }, build: { outDir: 'dist', emptyOutDir: true, sourcemap: false } };
});
