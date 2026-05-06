import * as esbuild from 'esbuild'

const watch = process.argv.includes('--watch')

const ctx = await esbuild.context({
  entryPoints: ['src/main.ts'],
  bundle     : true,
  outfile    : 'dist/renderer.js',
  format     : 'cjs',
  platform   : 'node',
  external   : ['electron'],
  sourcemap  : true,
  target     : 'es2022',
  logLevel   : 'info',
})

if (watch) {
  await ctx.watch()
} else {
  await ctx.rebuild()
  await ctx.dispose()
}

