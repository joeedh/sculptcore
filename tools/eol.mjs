// Working-tree line endings, as git would check a file out with them.
// Shared by genTS.ts and make.mjs so every generator in the repo writes the
// same endings (sbrushc has its own copy of this logic in sbrushc_main.cc).

import {execSync} from 'child_process'
import os from 'os'

function gitConfig(key, cwd) {
  try {
    return execSync(`git config --get ${key}`, {
      cwd,
      encoding: 'utf8',
      stdio   : ['ignore', 'pipe', 'ignore'],
    }).trim()
  } catch {
    // unset (exit 1) or git unavailable
    return ''
  }
}

export function getNativeEOL(cwd = process.cwd()) {
  // core.eol pins the working-tree ending outright and wins over core.autocrlf.
  const coreEol = gitConfig('core.eol', cwd)
  if (coreEol === 'lf') {
    return '\n'
  }
  if (coreEol === 'crlf') {
    return '\r\n'
  }

  const autocrlf = gitConfig('core.autocrlf', cwd)
  if (autocrlf === 'input' || autocrlf === 'false') {
    return '\n'
  }
  // 'true', or unset with core.eol=native → OS-native EOL
  return os.EOL === '\r\n' ? '\r\n' : '\n'
}

export function toEOL(text, eol) {
  return text.replace(/\r\n?|\n/g, eol)
}
