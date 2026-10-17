/**
 * Out-of-tree client bundle for dsh-eval-guard.
 *
 * Mirrors the two contracts the Web client's module loader requires: the
 * window.__ModuleLoader__.load closure handoff, and externals resolved through
 * the injected require (the loader module table). The client uses only React
 * and inline styles, so no CSS pipeline is involved.
 */
const ID = 'dsh-eval-guard'

/** Browser platform module-table rows this bundle may import. */
const PLATFORM_MODULE = /^(?:react|react\/jsx-runtime|react-dom|react-dom\/client|@deepseek-ai\/cordis|@deepseek-ai\/dsh-client-store|@deepseek-ai\/dsh-client-ui-slots|@deepseek-ai\/dsh-client-ui-primitives|@deepseek-ai\/dsh-client-ui-dockkit)$/

/** Node half: the host-side plugin entry. */
const hostHalf = {
  name: ID,
  entry: { index: 'src/index.ts' },
  outDir: 'lib',
  format: ['esm'],
  platform: 'node',
  target: 'es2024',
  fixedExtension: false,
  dts: false,
  clean: false,
}

/** Browser half: the closure-factory artifact the Web client's loader executes. */
const clientHalf = {
  name: `${ID}/client`,
  entry: { index: 'src/client/index.ts' },
  outDir: 'lib/client',
  format: 'cjs',
  platform: 'browser',
  target: 'es2024',
  fixedExtension: false,
  dts: false,
  clean: false,
  sourcemap: true,
  deps: {
    neverBundle: (specifier) => PLATFORM_MODULE.test(specifier),
    alwaysBundle: (specifier) => !PLATFORM_MODULE.test(specifier),
  },
  define: {
    'process.env.NODE_ENV': JSON.stringify('production'),
    'import.meta.env.MODE': JSON.stringify('production'),
    'import.meta.env': JSON.stringify({ MODE: 'production' }),
  },
  outputOptions: {
    entryFileNames: 'index.js',
    banner: (chunk) => `window.__ModuleLoader__.load({ id: ${JSON.stringify(ID)}, ${chunk.isEntry ? '' : `chunk: ${JSON.stringify(chunk.fileName)}, `}factory: (require) => {`,
    footer: 'return module.exports; } });',
    intro: 'var module = { exports: {} }; var exports = module.exports;',
  },
}

export default [hostHalf, clientHalf]
