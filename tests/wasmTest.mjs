import _wasm from '../build/sculptcore.js';
const wasm = await _wasm();

for (let k in wasm) {
    if (typeof k !== 'string') {
        continue
    }
    if (k.startsWith('_')) {
        wasm[k.slice(1)] = wasm[k]
    }
}

console.log(wasm)