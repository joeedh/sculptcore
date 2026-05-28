// Loads the clang-built .node in the Electron main process, exercises it, and
// writes the result to spike-result.json. N-API addons resolve their symbols
// from the host process, so requiring it in the main process is enough to prove
// the link/ABI works under Electron. Run:
//   <electron> sculptcore/spike/napi/electron-main.js
const {app} = require('electron')
const fs = require('fs')
const path = require('path')

function runTest() {
  const result = {}
  try {
    const addon = require('./build/sculptcore_napi_spike.node')
    result.hello = addon.hello()
    result.add = addon.add(2, 40)
    result.rawAdd = addon.rawAdd(2, 40)
    result.compiler = addon.compiler()
    result.ok = true
  } catch (e) {
    result.ok = false
    result.error = (e && e.stack) || String(e)
  }
  fs.writeFileSync(path.join(__dirname, 'spike-result.json'), JSON.stringify(result, null, 2))
  console.log('[napi-spike]', JSON.stringify(result))
  app.quit()
}

app.whenReady().then(runTest)
