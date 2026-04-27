import {termColor} from '../source/litestl/tests/termColor.js'

/** applies syntax highlighting to C++ code with termColor */
const keywords = new Set([
  'alignas',
  'and',
  'asm',
  'auto',
  'break',
  'case',
  'catch',
  'class',
  'concept',
  'const',
  'consteval',
  'constexpr',
  'constinit',
  'const_cast',
  'continue',
  'co_await',
  'co_return',
  'co_yield',
  'decltype',
  'default',
  'delete',
  'do',
  'dynamic_cast',
  'else',
  'enum',
  'explicit',
  'export',
  'extern',
  'false',
  'for',
  'friend',
  'goto',
  'if',
  'inline',
  'mutable',
  'namespace',
  'new',
  'noexcept',
  'not',
  'nullptr',
  'operator',
  'or',
  'private',
  'protected',
  'public',
  'register',
  'reinterpret_cast',
  'requires',
  'return',
  'sizeof',
  'static',
  'static_assert',
  'static_cast',
  'struct',
  'switch',
  'template',
  'this',
  'thread_local',
  'throw',
  'true',
  'try',
  'typedef',
  'typeid',
  'typename',
  'union',
  'using',
  'virtual',
  'volatile',
  'while',
  'xor',
])
const types = new Set([
  'bool',
  'char',
  'char8_t',
  'char16_t',
  'char32_t',
  'double',
  'float',
  'int',
  'long',
  'short',
  'signed',
  'unsigned',
  'void',
  'wchar_t',
  'size_t',
  'ssize_t',
  'ptrdiff_t',
  'int8_t',
  'int16_t',
  'int32_t',
  'int64_t',
  'uint8_t',
  'uint16_t',
  'uint32_t',
  'uint64_t',
])
export const otherKeywords = new Set([
  'offsetof', //
  'sizeof',
  'alignof',
  'std',
  'same_as',
  'derived_from',
  '__attribute__',
  'is_same_v',
  'is_base_of_v',
])

export function syntaxHighlight(str) {
  let out = ''
  let i = 0
  const n = str.length

  while (i < n) {
    const c = str[i]

    // line comment
    if (c === '/' && str[i + 1] === '/') {
      let j = i
      while (j < n && str[j] !== '\n') j++
      out += termColor(str.slice(i, j), 'gray')
      i = j
      continue
    }
    // block comment
    if (c === '/' && str[i + 1] === '*') {
      let j = i + 2
      while (j < n && !(str[j] === '*' && str[j + 1] === '/')) j++
      if (j < n) j += 2
      out += termColor(str.slice(i, j), 'gray')
      i = j
      continue
    }
    // preprocessor (only meaningful at line start, but ok to tag # token)
    if (c === '#') {
      let j = i + 1
      while (j < n && /[A-Za-z_]/.test(str[j])) j++
      out += termColor(str.slice(i, j), 'magenta')
      i = j
      continue
    }
    // string / char literal
    if (c === '"' || c === "'") {
      const quote = c
      let j = i + 1
      while (j < n && str[j] !== quote) {
        if (str[j] === '\\' && j + 1 < n) j += 2
        else j++
      }
      if (j < n) j++
      out += termColor(str.slice(i, j), 'green')
      i = j
      continue
    }
    // number
    if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(str[i + 1]))) {
      let j = i
      while (j < n && /[0-9a-fA-FxXuUlLfF.']/.test(str[j])) j++
      out += termColor(str.slice(i, j), 'orange')
      i = j
      continue
    }
    //operators
    if (/[<>]/.test(c)) {
      out += termColor(c, 'blue', 1)
      i++
      continue
    }
    if (/[\[\]{}()]/.test(c)) {
      out += termColor(c, 'yellow', 1)
      i++
      continue
    }
    if (/[+\-*/=!&|~^%:,;]/.test(c)) {
      out += termColor(c, 'grey', 1)
      i++
      continue
    }
    // identifier / keyword
    if (/[A-Za-z_]/.test(c)) {
      let j = i
      while (j < n && /[A-Za-z0-9_]/.test(str[j])) j++
      const word = str.slice(i, j)
      if (keywords.has(word)) {
        out += termColor(word, 'blue')
      } else if (types.has(word)) {
        out += termColor(word, 'teal')
      } else if (otherKeywords.has(word)) {
        out += termColor(word, 'teal')
      } else {
        out += word
      }
      i = j
      continue
    }
    out += c
    i++
  }
  return out
}
