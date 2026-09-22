#!/usr/bin/env node
/**
 * compare-usda.js
 *
 * A CLI tool to compare USDA (USD ASCII) outputs from lusdcat and usdcat.
 * Compares at the Prim and Attribute level, ignoring ordering differences.
 *
 * Usage:
 *   node compare-usda.js <file1.usda> <file2.usda>
 *   node compare-usda.js --lusdcat <lusdcat_path> --usdcat <usdcat_path> <input.usd>
 */

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

/**
 * Simple glob pattern matcher
 * Supports: *, **, ?, [abc], [a-z]
 */
function globToRegex(pattern) {
  let regex = '';
  let i = 0;

  while (i < pattern.length) {
    const c = pattern[i];

    if (c === '*') {
      if (pattern[i + 1] === '*') {
        // ** matches any path including /
        if (pattern[i + 2] === '/' || pattern[i + 2] === path.sep) {
          regex += '(?:.*\\/)?';
          i += 3;
        } else {
          regex += '.*';
          i += 2;
        }
      } else {
        // * matches anything except /
        regex += '[^\\/]*';
        i++;
      }
    } else if (c === '?') {
      regex += '[^\\/]';
      i++;
    } else if (c === '[') {
      // Character class
      let j = i + 1;
      let classContent = '';
      while (j < pattern.length && pattern[j] !== ']') {
        classContent += pattern[j];
        j++;
      }
      regex += '[' + classContent + ']';
      i = j + 1;
    } else if (c === '{') {
      // Brace expansion {a,b,c}
      let j = i + 1;
      let options = [];
      let current = '';
      while (j < pattern.length && pattern[j] !== '}') {
        if (pattern[j] === ',') {
          options.push(current);
          current = '';
        } else {
          current += pattern[j];
        }
        j++;
      }
      options.push(current);
      regex += '(?:' + options.map(o => escapeRegex(o)).join('|') + ')';
      i = j + 1;
    } else if ('/\\^$.|+()'.includes(c)) {
      regex += '\\' + c;
      i++;
    } else {
      regex += c;
      i++;
    }
  }

  return new RegExp('^' + regex + '$');
}

function escapeRegex(str) {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Check if a string contains glob pattern characters
 */
function isGlobPattern(str) {
  return /[*?[\]{}]/.test(str);
}

/**
 * Expand a glob pattern to matching file paths
 */
function expandGlob(pattern, baseDir = '.') {
  const results = [];
  const absolutePattern = path.isAbsolute(pattern) ? pattern : path.join(baseDir, pattern);

  // Find the first directory without glob characters
  const parts = absolutePattern.split(path.sep);
  let staticParts = [];
  let globParts = [];
  let inGlob = false;

  for (const part of parts) {
    if (inGlob || isGlobPattern(part)) {
      inGlob = true;
      globParts.push(part);
    } else {
      staticParts.push(part);
    }
  }

  const staticPath = staticParts.join(path.sep) || '/';
  const globPattern = globParts.join('/');

  if (!globPattern) {
    // No glob pattern, just check if file exists
    if (fs.existsSync(absolutePattern)) {
      return [absolutePattern];
    }
    return [];
  }

  const regex = globToRegex(globPattern);

  function walk(dir, relativePath = '') {
    if (!fs.existsSync(dir)) return;

    let entries;
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch (e) {
      return;
    }

    for (const entry of entries) {
      const entryRelPath = relativePath ? relativePath + '/' + entry.name : entry.name;
      const fullPath = path.join(dir, entry.name);

      if (entry.isDirectory()) {
        // Check if pattern could match in subdirectory
        if (globPattern.includes('**') || regex.test(entryRelPath + '/')) {
          walk(fullPath, entryRelPath);
        } else {
          // Check if directory name matches prefix of pattern
          const dirPattern = globPattern.split('/')[0];
          if (globToRegex(dirPattern).test(entry.name)) {
            walk(fullPath, entryRelPath);
          }
        }
      } else if (entry.isFile()) {
        if (regex.test(entryRelPath)) {
          results.push(fullPath);
        }
      }
    }
  }

  walk(staticPath);
  return results.sort();
}

/**
 * Expand glob patterns in file list
 */
function expandFilePatterns(patterns, baseDir = '.') {
  const files = [];

  for (const pattern of patterns) {
    if (isGlobPattern(pattern)) {
      let matched = expandGlob(pattern, baseDir);
      if (matched.length === 0) {
        console.warn(`Warning: No files matched pattern "${pattern}"`);
      }
      // Skip test-generated runtime artifacts (see .gitignore). They are
      // emitted by runtime tests, not roundtrip fixtures. Still allowed when
      // passed explicitly (non-glob) for targeted debugging.
      matched = matched.filter(f => !/-runtime\.usdc$/.test(f));
      files.push(...matched);
    } else {
      files.push(pattern);
    }
  }

  // Remove duplicates
  return [...new Set(files)];
}

/**
 * Half-precision (fp16/binary16) utilities
 * IEEE 754 half-precision: 1 sign bit, 5 exponent bits, 10 mantissa bits
 */

/**
 * Convert a float (parsed from string) to half-precision binary representation
 * Returns the 16-bit unsigned integer representation
 */
function floatToHalfBits(f) {
  // Handle special cases
  if (isNaN(f)) {
    return 0x7E00; // Canonical NaN
  }
  if (!isFinite(f)) {
    return f > 0 ? 0x7C00 : 0xFC00; // +inf or -inf
  }
  if (f === 0) {
    // Check for -0
    return Object.is(f, -0) ? 0x8000 : 0x0000;
  }

  const sign = f < 0 ? 1 : 0;
  f = Math.abs(f);

  // Get the float32 representation to extract components
  const float32View = new Float32Array(1);
  const uint32View = new Uint32Array(float32View.buffer);
  float32View[0] = f;
  const bits32 = uint32View[0];

  const exp32 = (bits32 >> 23) & 0xFF;
  const mant32 = bits32 & 0x7FFFFF;

  let exp16, mant16;

  if (exp32 === 0) {
    // Float32 denormal -> Half denormal or zero
    exp16 = 0;
    mant16 = 0;
  } else if (exp32 === 0xFF) {
    // Inf or NaN
    exp16 = 0x1F;
    mant16 = mant32 ? 0x200 : 0; // NaN or Inf
  } else {
    // Normal number
    const newExp = exp32 - 127 + 15; // Rebias from float32 to half

    if (newExp >= 0x1F) {
      // Overflow -> Infinity
      exp16 = 0x1F;
      mant16 = 0;
    } else if (newExp <= 0) {
      // Underflow -> denormal or zero
      if (newExp < -10) {
        // Too small, becomes zero
        exp16 = 0;
        mant16 = 0;
      } else {
        // Denormal
        exp16 = 0;
        // Add implicit leading 1 and shift
        const shift = 1 - newExp;
        mant16 = ((mant32 | 0x800000) + (1 << (shift + 12))) >> (shift + 13);
      }
    } else {
      // Normal number
      exp16 = newExp;
      // Round mantissa from 23 bits to 10 bits
      mant16 = (mant32 + 0x1000) >> 13; // Round to nearest
      if (mant16 & 0x400) {
        // Mantissa overflow due to rounding
        mant16 = 0;
        exp16++;
        if (exp16 >= 0x1F) {
          exp16 = 0x1F;
          mant16 = 0; // Overflow to infinity
        }
      }
    }
  }

  return (sign << 15) | (exp16 << 10) | (mant16 & 0x3FF);
}

/**
 * Convert half-precision bits back to float for display
 */
function halfBitsToFloat(bits) {
  const sign = (bits >> 15) & 1;
  const exp = (bits >> 10) & 0x1F;
  const mant = bits & 0x3FF;

  let value;
  if (exp === 0) {
    // Denormal or zero
    value = mant * Math.pow(2, -24); // 2^(-14-10)
  } else if (exp === 0x1F) {
    // Inf or NaN
    value = mant ? NaN : Infinity;
  } else {
    // Normal number
    value = (1 + mant / 1024) * Math.pow(2, exp - 15);
  }

  return sign ? -value : value;
}

/**
 * Check if a string value appears to be a half-precision special value
 */
function isHalfSpecialValue(str) {
  const lower = str.toLowerCase().trim();
  return lower === 'nan' || lower === 'inf' || lower === '-inf' ||
         lower === '+inf' || lower === 'infinity' || lower === '-infinity';
}

/**
 * Compare two numeric values as half-precision floats
 * Returns true if they have the same binary representation
 */
function areHalfValuesEqual(val1, val2) {
  // Handle special values (nan, inf) as string comparison
  const isSpecial1 = isHalfSpecialValue(String(val1));
  const isSpecial2 = isHalfSpecialValue(String(val2));

  if (isSpecial1 || isSpecial2) {
    // For nan/inf, compare as strings (normalized)
    const norm1 = String(val1).toLowerCase().trim();
    const norm2 = String(val2).toLowerCase().trim();
    // Normalize inf variants
    const normalize = s => s.replace(/^\+/, '').replace(/^infinity$/, 'inf').replace(/^-infinity$/, '-inf');
    return normalize(norm1) === normalize(norm2);
  }

  // Parse as floats and convert to half binary
  const f1 = parseFloat(val1);
  const f2 = parseFloat(val2);

  if (isNaN(f1) || isNaN(f2)) {
    return false;
  }

  const bits1 = floatToHalfBits(f1);
  const bits2 = floatToHalfBits(f2);

  return bits1 === bits2;
}

/**
 * Check if an attribute name suggests half-precision type
 * Half types in USD: half, half2, half3, half4, quath (quaternion half)
 */
function isHalfPrecisionAttribute(attrName) {
  // Match type annotations like "half", "half2", "half3", "half4", "quath"
  // Also match array types like "half[]", "half3[]", "quath[]"
  const halfTypePattern = /\b(half[234]?|quath)\b/i;
  return halfTypePattern.test(attrName);
}

/**
 * Recursively compare two values as half-precision
 * Handles scalars, tuples (half2, half3, half4), and arrays
 */
function areValuesEqualAsHalf(val1, val2) {
  // Handle null/undefined
  if (val1 === null || val1 === undefined || val2 === null || val2 === undefined) {
    return val1 === val2;
  }

  // Handle objects (parsed USDA values)
  if (typeof val1 === 'object' && typeof val2 === 'object') {
    // Both are tuples (e.g., half3 = (1.0, 2.0, 3.0))
    if (val1.type === 'tuple' && val2.type === 'tuple') {
      if (val1.value.length !== val2.value.length) return false;
      for (let i = 0; i < val1.value.length; i++) {
        if (!areValuesEqualAsHalf(val1.value[i], val2.value[i])) {
          return false;
        }
      }
      return true;
    }

    // Both are arrays (e.g., half3[] = [(1,2,3), (4,5,6)])
    if (val1.type === 'array' && val2.type === 'array') {
      if (val1.value.length !== val2.value.length) return false;
      for (let i = 0; i < val1.value.length; i++) {
        if (!areValuesEqualAsHalf(val1.value[i], val2.value[i])) {
          return false;
        }
      }
      return true;
    }

    // Both are numbers
    if (val1.type === 'number' && val2.type === 'number') {
      return areHalfValuesEqual(val1.value, val2.value);
    }

    // Mixed types - not equal
    if (val1.type !== val2.type) return false;

    // Other types - compare normally
    return normalizeValue(val1) === normalizeValue(val2);
  }

  // Handle primitive strings (raw number strings)
  if (typeof val1 === 'string' && typeof val2 === 'string') {
    // Try to parse as numbers and compare as half
    const num1 = parseFloat(val1);
    const num2 = parseFloat(val2);
    if (!isNaN(num1) && !isNaN(num2)) {
      return areHalfValuesEqual(val1, val2);
    }
    // Non-numeric strings - compare directly
    return val1 === val2;
  }

  // Fallback to string comparison
  return String(val1) === String(val2);
}

// USDA Token types
const TokenType = {
  IDENTIFIER: 'IDENTIFIER',
  STRING: 'STRING',
  NUMBER: 'NUMBER',
  LBRACE: 'LBRACE',
  RBRACE: 'RBRACE',
  LPAREN: 'LPAREN',
  RPAREN: 'RPAREN',
  LBRACKET: 'LBRACKET',
  RBRACKET: 'RBRACKET',
  EQUALS: 'EQUALS',
  COLON: 'COLON',
  DOT: 'DOT',
  COMMA: 'COMMA',
  SEMICOLON: 'SEMICOLON',
  AT: 'AT',
  COMMENT: 'COMMENT',
  NEWLINE: 'NEWLINE',
  EOF: 'EOF',
};

/**
 * Simple USDA Lexer
 */
class UsdaLexer {
  constructor(input) {
    this.input = input;
    this.pos = 0;
    this.line = 1;
    this.col = 1;
  }

  peek(offset = 0) {
    return this.input[this.pos + offset];
  }

  advance() {
    const ch = this.input[this.pos++];
    if (ch === '\n') {
      this.line++;
      this.col = 1;
    } else {
      this.col++;
    }
    return ch;
  }

  skipWhitespace() {
    while (this.pos < this.input.length) {
      const ch = this.peek();
      if (ch === ' ' || ch === '\t' || ch === '\r') {
        this.advance();
      } else if (ch === '\n') {
        this.advance();
      } else if (ch === '#') {
        // Skip comments
        while (this.pos < this.input.length && this.peek() !== '\n') {
          this.advance();
        }
      } else {
        break;
      }
    }
  }

  readString() {
    const quote = this.advance(); // consume opening quote
    let value = '';
    let isTriple = false;

    // Check for triple-quoted string
    if (this.peek() === quote && this.peek(1) === quote) {
      this.advance();
      this.advance();
      isTriple = true;
    }

    while (this.pos < this.input.length) {
      const ch = this.peek();

      if (isTriple) {
        if (ch === quote && this.peek(1) === quote && this.peek(2) === quote) {
          this.advance();
          this.advance();
          this.advance();
          break;
        }
      } else {
        if (ch === quote) {
          this.advance();
          break;
        }
        if (ch === '\n') {
          break; // Unterminated string
        }
      }

      if (ch === '\\') {
        this.advance();
        const next = this.advance();
        switch (next) {
          case 'n': value += '\n'; break;
          case 't': value += '\t'; break;
          case 'r': value += '\r'; break;
          case '\\': value += '\\'; break;
          case '"': value += '"'; break;
          case "'": value += "'"; break;
          default: value += next;
        }
      } else {
        value += this.advance();
      }
    }

    return { type: TokenType.STRING, value };
  }

  readAssetPath() {
    this.advance(); // consume opening @
    let value = '';

    // Check if it's a triple-@ delimited path @@@...@@@
    let isTripleDelim = false;
    if (this.peek() === '@' && this.peek(1) === '@') {
      // It's @@@...@@@
      this.advance(); // consume second @
      this.advance(); // consume third @
      isTripleDelim = true;
    }

    // Check if it's a quoted asset path @"..."@ or @'...'@
    if (!isTripleDelim && (this.peek() === '"' || this.peek() === "'")) {
      const quote = this.advance();
      while (this.pos < this.input.length) {
        const ch = this.peek();
        if (ch === quote) {
          this.advance();
          break;
        }
        if (ch === '\\') {
          this.advance();
          value += this.advance();
        } else {
          value += this.advance();
        }
      }
    } else {
      // Unquoted asset path @path@ or @@@path@@@
      while (this.pos < this.input.length) {
        const ch = this.peek();

        // Handle escaped characters (including \@@@)
        if (ch === '\\') {
          // Check what's being escaped
          const nextCh = this.peek(1);

          if (nextCh === '\\') {
            // \\ = one escaped backslash
            this.advance(); // consume first backslash
            this.advance(); // consume second backslash
            value += '\\';
            continue;
          } else if (isTripleDelim && nextCh === '@' && this.peek(2) === '@' && this.peek(3) === '@') {
            // \@@@ = escape the @@@ sequence, include three literal @ characters
            // This allows @@@...0/teapot_\@@@n.png@@@@ to have the @@@n part
            this.advance(); // consume backslash
            value += this.advance(); // add first @
            value += this.advance(); // add second @
            value += this.advance(); // add third @
            continue;
          } else {
            // Regular escape handling for other characters
            this.advance();
            value += this.advance();
            continue;
          }
        }

        if (isTripleDelim) {
          // For @@@...@@@, look for closing @@@
          // But only if followed by whitespace, delimiter, or EOF (not alphanumeric content)
          if (ch === '@' && this.peek(1) === '@' && this.peek(2) === '@') {
            const afterTriple = this.peek(3);
            // Check if this is truly the closing @@@ or part of the content
            // Closing @@@ should be followed by whitespace, newline, delimiter, or EOF
            if (afterTriple === undefined || afterTriple === ' ' || afterTriple === '\t' ||
                afterTriple === '\n' || afterTriple === '\r' || afterTriple === ')' ||
                afterTriple === '}' || afterTriple === ']' || afterTriple === ',' ||
                afterTriple === ';' || afterTriple === '#') {
              this.advance(); // consume first @
              this.advance(); // consume second @
              this.advance(); // consume third @
              break;
            }
            // Otherwise, @@@ is part of the content - include it in value
            value += this.advance();
            value += this.advance();
            value += this.advance();
            continue;
          }
        } else {
          // For @...@, look for closing @
          if (ch === '@') {
            this.advance();
            break;
          }
        }

        value += this.advance();
      }
    }

    return { type: TokenType.STRING, value, isAsset: true };
  }

  readNumber() {
    let value = '';
    let hasDecimal = false;
    let hasExponent = false;

    // Handle sign
    if (this.peek() === '-' || this.peek() === '+') {
      value += this.advance();
    }

    // Handle special values: inf, nan
    if (this.peek() === 'i' || this.peek() === 'n') {
      const rest = this.input.slice(this.pos, this.pos + 3);
      if (rest === 'inf' || rest === 'nan') {
        value += this.advance() + this.advance() + this.advance();
        return { type: TokenType.NUMBER, value };
      }
    }

    while (this.pos < this.input.length) {
      const ch = this.peek();
      if (/[0-9]/.test(ch)) {
        value += this.advance();
      } else if (ch === '.' && !hasDecimal && !hasExponent) {
        hasDecimal = true;
        value += this.advance();
      } else if ((ch === 'e' || ch === 'E') && !hasExponent) {
        hasExponent = true;
        value += this.advance();
        if (this.peek() === '+' || this.peek() === '-') {
          value += this.advance();
        }
      } else {
        break;
      }
    }

    return { type: TokenType.NUMBER, value };
  }

  readIdentifier() {
    let value = '';
    while (this.pos < this.input.length) {
      const ch = this.peek();
      if (/[a-zA-Z0-9_]/.test(ch)) {
        value += this.advance();
      } else {
        break;
      }
    }
    return { type: TokenType.IDENTIFIER, value };
  }

  readConnection() {
    this.advance(); // consume opening <
    let value = '';

    while (this.pos < this.input.length) {
      const ch = this.peek();

      if (ch === '>') {
        this.advance();
        break;
      }

      if (ch === '\\') {
        this.advance();
        value += this.advance();
      } else {
        value += this.advance();
      }
    }

    return { type: TokenType.STRING, value };
  }

  nextToken() {
    this.skipWhitespace();

    if (this.pos >= this.input.length) {
      return { type: TokenType.EOF, value: '', line: this.line, col: this.col };
    }

    const startLine = this.line;
    const startCol = this.col;
    const ch = this.peek();

    // Single character tokens
    switch (ch) {
      case '{': this.advance(); return { type: TokenType.LBRACE, value: '{', line: startLine, col: startCol };
      case '}': this.advance(); return { type: TokenType.RBRACE, value: '}', line: startLine, col: startCol };
      case '(': this.advance(); return { type: TokenType.LPAREN, value: '(', line: startLine, col: startCol };
      case ')': this.advance(); return { type: TokenType.RPAREN, value: ')', line: startLine, col: startCol };
      case '[': this.advance(); return { type: TokenType.LBRACKET, value: '[', line: startLine, col: startCol };
      case ']': this.advance(); return { type: TokenType.RBRACKET, value: ']', line: startLine, col: startCol };
      case '=': this.advance(); return { type: TokenType.EQUALS, value: '=', line: startLine, col: startCol };
      case ':': this.advance(); return { type: TokenType.COLON, value: ':', line: startLine, col: startCol };
      case '.': this.advance(); return { type: TokenType.DOT, value: '.', line: startLine, col: startCol };
      case ',': this.advance(); return { type: TokenType.COMMA, value: ',', line: startLine, col: startCol };
      case ';': this.advance(); return { type: TokenType.SEMICOLON, value: ';', line: startLine, col: startCol };
      case '@': {
        const token = this.readAssetPath();
        return { ...token, line: startLine, col: startCol };
      }
      case '<': {
        const token = this.readConnection();
        return { ...token, line: startLine, col: startCol };
      }
    }

    // Strings
    if (ch === '"' || ch === "'") {
      const token = this.readString();
      return { ...token, line: startLine, col: startCol };
    }

    // Numbers (including negative)
    if (/[0-9]/.test(ch) || ((ch === '-' || ch === '+') && /[0-9.]/.test(this.peek(1)))) {
      const token = this.readNumber();
      return { ...token, line: startLine, col: startCol };
    }

    // Identifiers and keywords
    if (/[a-zA-Z_]/.test(ch)) {
      const token = this.readIdentifier();
      return { ...token, line: startLine, col: startCol };
    }

    // Skip unknown characters
    this.advance();
    return this.nextToken();
  }

  tokenize() {
    const tokens = [];
    let token;
    while ((token = this.nextToken()).type !== TokenType.EOF) {
      tokens.push(token);
    }
    return tokens;
  }
}

/**
 * USDA Parser - parses into a structured representation
 */
class UsdaParser {
  constructor(tokens) {
    this.tokens = tokens;
    this.pos = 0;
    this.iterations = 0;
    this.maxIterations = 5000000;  // Safety limit — abort if parser gets stuck
  }

  checkIterations() {
    if (++this.iterations > this.maxIterations) {
      throw new Error(`Parser iteration limit exceeded (${this.maxIterations})`);
    }
  }

  // Check if current token starts a relationship declaration
  // Valid starts: rel, custom rel, delete rel, prepend rel, append rel, add rel, reorder rel
  // Also: append custom rel, delete custom rel, etc.
  isRelationshipStart(tokenValue) {
    if (tokenValue === 'rel') return true;

    const relQualifiers = ['custom', 'delete', 'prepend', 'append', 'add', 'reorder'];
    if (!relQualifiers.includes(tokenValue)) return false;

    // Look ahead to find 'rel' (may be after other qualifiers)
    let offset = 1;
    while (offset < 5) { // Max 4 qualifiers before 'rel'
      const nextToken = this.peek(offset);
      if (!nextToken || nextToken.type !== TokenType.IDENTIFIER) return false;
      if (nextToken.value === 'rel') return true;
      if (!relQualifiers.includes(nextToken.value)) return false;
      offset++;
    }
    return false;
  }

  peek(offset = 0) {
    const idx = this.pos + offset;
    if (idx >= this.tokens.length) {
      return { type: TokenType.EOF, value: '', line: -1, col: -1 };
    }
    const token = this.tokens[idx];
    // Ensure token always has required properties
    if (!token) {
      return { type: TokenType.EOF, value: '', line: -1, col: -1 };
    }
    return token;
  }

  advance() {
    const token = this.tokens[this.pos++];
    if (!token) {
      return { type: TokenType.EOF, value: '', line: -1, col: -1 };
    }
    return token;
  }

  expect(type) {
    const token = this.advance();
    if (token.type !== type) {
      const tokenStr = token ? `${token.type}(${token.value})` : 'EOF()';
      const context = this.getTokenContext();
      throw new Error(`Expected ${type} but got ${tokenStr} at position ${this.pos}\nContext:\n${context}`);
    }
    return token;
  }

  getTokenContext(offset = 0) {
    const startIdx = Math.max(0, this.pos - 2);
    const endIdx = Math.min(this.tokens.length, this.pos + 3);
    const tokens = this.tokens.slice(startIdx, endIdx);
    return tokens.map((t, i) => {
      const marker = startIdx + i === this.pos ? '>>> ' : '    ';
      return `${marker}[${startIdx + i}] ${t ? t.type : 'UNDEFINED'}('${t ? t.value : ''}')`;
    }).join('\n');
  }

  match(type, value = null) {
    const token = this.peek();
    if (token.type === type && (value === null || token.value === value)) {
      return this.advance();
    }
    return null;
  }

  parse() {
    const result = {
      header: null,
      metadata: {},
      prims: []
    };

    try {
      // Parse header (e.g., #usda 1.0)
      const headerToken = this.peek();
      if (headerToken && headerToken.type === TokenType.IDENTIFIER && headerToken.value === 'usda') {
        this.advance();
        const versionToken = this.peek();
        if (versionToken && versionToken.type === TokenType.NUMBER) {
          result.header = { version: this.advance().value };
        }
      }

      // Parse top-level metadata
      const metaToken = this.peek();
      if (metaToken && metaToken.type === TokenType.LPAREN) {
        result.metadata = this.parseMetadata();
      }

      // Parse prims
      while (this.pos < this.tokens.length) {
        const token = this.peek();
        if (!token || token.type === TokenType.EOF) break;

        if (token.type === TokenType.IDENTIFIER &&
            (token.value === 'def' || token.value === 'over' || token.value === 'class')) {
          try {
            result.prims.push(this.parsePrim(''));
          } catch (e) {
            // If prim parsing fails, skip to next prim
            console.error('Error parsing prim:', e.message);
            const lines = e.message.split('\n');
            for (const line of lines) {
              if (line.startsWith('Expected') || line.includes('>>>')) {
                console.error('  ' + line);
              }
            }
            while (this.pos < this.tokens.length &&
                   !(this.peek() && this.peek().type === TokenType.IDENTIFIER &&
                     (this.peek().value === 'def' || this.peek().value === 'over' || this.peek().value === 'class'))) {
              this.advance();
            }
          }
        } else {
          this.advance(); // Skip unexpected tokens
        }
      }
    } catch (e) {
      console.error('Error in top-level parse:', e.message);
      // Return partial result
    }

    return result;
  }

  parseMetadata() {
    const metadata = {};
    this.expect(TokenType.LPAREN);

    while (this.peek().type !== TokenType.RPAREN && this.peek().type !== TokenType.EOF) {
      // Skip any unexpected tokens (e.g., leftover numbers from broken token sequences)
      if (this.peek().type === TokenType.NUMBER || this.peek().type === TokenType.DOT) {
        this.advance();
        continue;
      }

      const key = this.parseAttributeName();
      if (!key) {
        // If we can't parse an attribute name, skip this token and continue
        if (this.peek().type !== TokenType.RPAREN && this.peek().type !== TokenType.EOF) {
          this.advance();
        }
        continue;
      }

      if (this.match(TokenType.EQUALS)) {
        metadata[key] = this.parseValue();
      }
    }

    this.expect(TokenType.RPAREN);
    return metadata;
  }

  parseAttributeName() {
    let name = '';

    // Handle type prefix (e.g., "uniform", "custom")
    while (this.peek() && this.peek().type === TokenType.IDENTIFIER) {
      const token = this.peek();
      if (token && token.value && ['uniform', 'custom', 'varying', 'config', 'prepend', 'append', 'delete', 'add', 'reorder'].includes(token.value)) {
        name += this.advance().value + ' ';
      } else {
        break;
      }
    }

    // Handle type annotation (e.g., "float3", "token[]")
    const typeToken = this.peek();
    if (typeToken && typeToken.type === TokenType.IDENTIFIER) {
      name += this.advance().value;

      // Handle array type
      const bracketToken = this.peek();
      if (bracketToken && bracketToken.type === TokenType.LBRACKET) {
        this.advance();
        this.expect(TokenType.RBRACKET);
        name += '[]';
      }
    }

    // Handle attribute name with namespaces (e.g., "xformOp:translate")
    const nameToken = this.peek();
    if (nameToken && nameToken.type === TokenType.IDENTIFIER) {
      name += ' ' + this.advance().value;

      while (this.peek() && this.peek().type === TokenType.COLON) {
        this.advance();
        const colonToken = this.peek();
        if (colonToken && colonToken.type === TokenType.IDENTIFIER) {
          name += ':' + this.advance().value;
        } else {
          break;
        }
      }
    }

    return name.trim();
  }

  parseValue() {
    this.checkIterations();
    const token = this.peek();

    if (!token) {
      throw new Error(`Unexpected end of tokens while parsing value at position ${this.pos}`);
    }

    // Dictionary (e.g., assetInfo = { string name = "baked_mesh" })
    if (token.type === TokenType.LBRACE) {
      return this.parseDictionary();
    }

    // Array
    if (token.type === TokenType.LBRACKET) {
      return this.parseArray();
    }

    // Tuple/Vector
    if (token.type === TokenType.LPAREN) {
      return this.parseTuple();
    }

    // String or Asset reference
    if (token.type === TokenType.STRING) {
      const tok = this.advance();
      if (tok && tok.isAsset) {
        return { type: 'asset', value: tok.value };
      }
      return { type: 'string', value: tok && tok.value ? tok.value : '' };
    }

    // Number
    if (token.type === TokenType.NUMBER) {
      const numToken = this.advance();
      return { type: 'number', value: numToken && numToken.value ? numToken.value : '0' };
    }

    // Identifier (token, enum, etc.)
    if (token.type === TokenType.IDENTIFIER) {
      let value = this.advance().value;

      // Handle path references (e.g., </Root/Child>)
      if (value === 'None' || value === 'True' || value === 'False') {
        return { type: 'keyword', value };
      }

      return { type: 'token', value };
    }

    // Path reference
    if (token.type === TokenType.STRING || (token.value && token.value === '<')) {
      return this.parsePath();
    }

    return { type: 'unknown', value: token && token.value ? token.value : '?' };
  }

  parseDictionary() {
    this.expect(TokenType.LBRACE);
    const entries = {};

    while (this.peek().type !== TokenType.RBRACE && this.peek().type !== TokenType.EOF) {
      this.checkIterations();
      const savedPos = this.pos;
      // Dictionary entry format: type key = value
      // e.g., "string name = "baked_mesh""
      const key = this.parseDictionaryKey();
      if (this.match(TokenType.EQUALS)) {
        entries[key] = this.parseValue();
      }
      this.match(TokenType.COMMA);
      // Guard: if no tokens consumed, skip one token to avoid infinite loop
      if (this.pos === savedPos) {
        this.advance();
      }
    }

    this.expect(TokenType.RBRACE);
    return { type: 'dictionary', value: entries };
  }

  parseDictionaryKey() {
    let key = '';

    // Handle type prefix (e.g., "string", "int", "asset", "dictionary")
    const typeToken = this.peek();
    if (typeToken && typeToken.type === TokenType.IDENTIFIER) {
      const typeName = typeToken.value;
      key = typeName;

      this.advance();

      // Handle array type (e.g., "string[]")
      const bracketToken = this.peek();
      if (bracketToken && bracketToken.type === TokenType.LBRACKET) {
        this.advance();
        this.expect(TokenType.RBRACKET);
        key += '[]';
      }

      // Handle key name - can be identifier or string (path)
      const nameToken = this.peek();
      if (nameToken && nameToken.type === TokenType.IDENTIFIER) {
        key += ' ' + this.advance().value;
      } else if (nameToken && nameToken.type === TokenType.STRING) {
        const str = this.advance();
        key += ' ' + (str && str.isAsset ? '@' + str.value + '@' : '"' + (str && str.value ? str.value : '') + '"');
      }
    }

    return key.trim();
  }

  parseArray() {
    this.expect(TokenType.LBRACKET);
    const elements = [];

    while (this.peek().type !== TokenType.RBRACKET && this.peek().type !== TokenType.EOF) {
      this.checkIterations();
      const savedPos = this.pos;
      elements.push(this.parseValue());
      this.match(TokenType.COMMA);
      if (this.pos === savedPos) this.advance();
    }

    this.expect(TokenType.RBRACKET);
    return { type: 'array', value: elements };
  }

  parseTuple() {
    this.expect(TokenType.LPAREN);
    const elements = [];

    while (this.peek().type !== TokenType.RPAREN && this.peek().type !== TokenType.EOF) {
      this.checkIterations();
      const savedPos2 = this.pos;
      elements.push(this.parseValue());
      this.match(TokenType.COMMA);
      if (this.pos === savedPos2) this.advance();
    }

    this.expect(TokenType.RPAREN);
    return { type: 'tuple', value: elements };
  }

  parsePath() {
    // Simple path parsing for now
    let path = '';
    while (this.pos < this.tokens.length) {
      const token = this.peek();
      if (!token || token.type === TokenType.EOF ||
          token.type === TokenType.COMMA ||
          token.type === TokenType.RBRACKET ||
          token.type === TokenType.RPAREN ||
          token.type === TokenType.RBRACE ||
          token.type === TokenType.EQUALS) {
        break;
      }
      const val = token && token.value ? token.value : '';
      path += val;
      this.advance();
    }
    return { type: 'path', value: path };
  }

  parsePrim(parentPath) {
    const startToken = this.peek();
    const startLine = startToken && startToken.line ? startToken.line : -1;
    const specToken = this.advance();
    const specifier = specToken && specToken.value ? specToken.value : 'def'; // def, over, class

    let typeName = '';
    const typeToken = this.peek();
    if (typeToken && typeToken.type === TokenType.IDENTIFIER && typeToken.value !== 'None') {
      // Check if it's a type name or the prim name
      const nextToken = this.peek(1);
      if (nextToken && nextToken.type === TokenType.STRING) {
        typeName = this.advance().value;
      }
    }

    const nameToken = this.peek();
    const name = nameToken && nameToken.type === TokenType.STRING ? this.advance().value : '';
    const primPath = parentPath ? `${parentPath}/${name}` : `/${name}`;

    const prim = {
      specifier,
      typeName,
      name,
      path: primPath,
      startLine,
      metadata: {},
      attributes: {},
      relationships: {},
      children: []
    };

    // Parse prim metadata
    if (this.peek().type === TokenType.LPAREN) {
      prim.metadata = this.parseMetadata();
    }

    // Parse prim body
    if (this.peek().type === TokenType.LBRACE) {
      this.expect(TokenType.LBRACE);

      while (this.peek().type !== TokenType.RBRACE && this.peek().type !== TokenType.EOF) {
        this.checkIterations();
        const bodyPos = this.pos;
        const token = this.peek();

        // Variant bodies use prim-like syntax inside `variantSet "name" = { ... }`.
        // This comparison parser does not model variants, so skip the balanced
        // block explicitly instead of letting the generic attribute parser consume
        // braces and accidentally pop the parent prim scope.
        if (token.type === TokenType.IDENTIFIER && token.value === 'variantSet') {
          this.advance(); // variantSet
          if (this.peek().type === TokenType.STRING || this.peek().type === TokenType.IDENTIFIER) {
            this.advance(); // variant set name
          }
          this.match(TokenType.EQUALS);
          if (this.peek().type === TokenType.LBRACE) {
            this.skipBalancedBlock(TokenType.LBRACE, TokenType.RBRACE);
          }
          continue;
        }

        // Child prim
        if (token.type === TokenType.IDENTIFIER &&
            (token.value === 'def' || token.value === 'over' || token.value === 'class')) {
          prim.children.push(this.parsePrim(primPath));
          continue;
        }

        // Relationship (with optional qualifiers: custom, delete, prepend, append, add, reorder)
        // Valid patterns: rel X, custom rel X, delete rel X, append custom rel X, etc.
        if (token.type === TokenType.IDENTIFIER && this.isRelationshipStart(token.value)) {
          // Collect all qualifiers before 'rel'
          let qualifiers = '';
          while (this.peek() && this.peek().type === TokenType.IDENTIFIER && this.peek().value !== 'rel') {
            qualifiers += this.advance().value + ' ';
          }
          this.advance(); // consume 'rel'
          const relName = qualifiers + this.parseRelationshipName();
          if (this.match(TokenType.EQUALS)) {
            prim.relationships[relName] = this.parseValue();
          } else {
            prim.relationships[relName] = null;
          }
          continue;
        }

        // Attribute
        if (token.type === TokenType.IDENTIFIER) {
          const attrStartLine = token.line;
          const attrName = this.parseAttributeName();
          if (!attrName) {
            // Failed to parse attribute name, skip this token
            this.advance();
            continue;
          }

          if (this.match(TokenType.EQUALS)) {
            try {
              const value = this.parseValue();
              prim.attributes[attrName] = {
                value: value,
                line: attrStartLine
              };
            } catch (e) {
              // Skip on parse error
            }
          } else if (this.peek().type === TokenType.DOT) {
            // Handle time samples or connections
            this.advance();
            if (this.peek().type === TokenType.IDENTIFIER || this.peek().type === TokenType.STRING) {
              const qualifier = this.advance().value;
              if (this.match(TokenType.EQUALS)) {
                try {
                  const key = `${attrName}.${qualifier}`;
                  let value;
                  if (qualifier === 'timeSamples' && this.peek().type === TokenType.LBRACE) {
                    value = this.parseTimeSamples();
                  } else {
                    value = this.parseValue();
                  }
                  prim.attributes[key] = {
                    value: value,
                    line: attrStartLine
                  };
                } catch (e) {
                  // Skip on parse error
                }
              }
            }
          }
          continue;
        }

        // Skip unknown tokens
        this.advance();
      }

      this.expect(TokenType.RBRACE);
    }

    return prim;
  }

  skipBalancedBlock(openType, closeType) {
    this.expect(openType);
    let depth = 1;

    while (depth > 0 && this.peek().type !== TokenType.EOF) {
      this.checkIterations();
      const token = this.advance();
      if (token.type === openType) {
        depth++;
      } else if (token.type === closeType) {
        depth--;
      }
    }
  }

  parseRelationshipName() {
    let name = '';
    const firstToken = this.peek();
    if (firstToken && firstToken.type === TokenType.IDENTIFIER) {
      name = this.advance().value;
      while (this.peek() && this.peek().type === TokenType.COLON) {
        this.advance();
        const nextToken = this.peek();
        if (nextToken && nextToken.type === TokenType.IDENTIFIER) {
          name += ':' + this.advance().value;
        } else {
          break;
        }
      }
    }
    return name;
  }

  parseTimeSamples() {
    // Parse timeSamples dictionary: { timeCode: value, timeCode: value, ... }
    this.expect(TokenType.LBRACE);
    const entries = {};

    while (this.peek().type !== TokenType.RBRACE && this.peek().type !== TokenType.EOF) {
      // Parse time code (number or identifier)
      let timeCode = '';
      const timeToken = this.peek();

      if (timeToken.type === TokenType.NUMBER) {
        timeCode = this.advance().value;
      } else if (timeToken.type === TokenType.IDENTIFIER) {
        timeCode = this.advance().value;
      } else {
        // Skip malformed entry
        this.advance();
        continue;
      }

      // Expect colon
      if (!this.match(TokenType.COLON)) {
        continue;
      }

      // Parse value at this time code
      try {
        entries[timeCode] = this.parseValue();
      } catch (e) {
        // Skip on parse error
      }

      this.match(TokenType.COMMA);
    }

    this.expect(TokenType.RBRACE);
    return { type: 'timeSamples', value: entries };
  }

  skipComplexValue() {
    // Skip over complex values like timeSamples dictionaries
    let braceDepth = 0;
    let bracketDepth = 0;
    let parenDepth = 0;

    while (this.pos < this.tokens.length) {
      const token = this.peek();

      if (token.type === TokenType.LBRACE) braceDepth++;
      else if (token.type === TokenType.RBRACE) {
        braceDepth--;
        if (braceDepth === -1) break;
      } else if (token.type === TokenType.LBRACKET) bracketDepth++;
      else if (token.type === TokenType.RBRACKET) bracketDepth--;
      else if (token.type === TokenType.LPAREN) parenDepth++;
      else if (token.type === TokenType.RPAREN) parenDepth--;

      if (braceDepth === 0 && bracketDepth === 0 && parenDepth === 0 &&
          (token.type === TokenType.COMMA || token.type === TokenType.RBRACE ||
           token.type === TokenType.RBRACKET || token.type === TokenType.RPAREN)) {
        break;
      }

      this.advance();
    }
  }
}

/**
 * Normalize a value for comparison
 */
function normalizeValue(val) {
  // Extract actual value if it's wrapped with location info
  if (val && typeof val === 'object' && val.value !== undefined && val.line !== undefined) {
    val = val.value;
  }

  if (val === null || val === undefined) {
    return 'null';
  }

  if (typeof val !== 'object') {
    return String(val);
  }

  if (val.type === 'number') {
    // Normalize number representation - treat 1, 1.0, 1e0 as same
    const num = parseFloat(val.value);
    if (isNaN(num)) return val.value;

    // For integers, return as integer to match "1" with "1.0"
    if (Number.isInteger(num)) {
      return String(num);
    }

    // For decimals, use normalized fixed precision
    // Round to 6 decimal places and remove trailing zeros
    return num.toFixed(6).replace(/\.?0+$/, '');
  }

  if (val.type === 'string' || val.type === 'token' || val.type === 'keyword') {
    return val.value;
  }

  if (val.type === 'array') {
    // Normalize each element and sort for consistent comparison (for unordered arrays)
    const normalized = val.value.map(normalizeValue);
    return '[' + normalized.join(', ') + ']';
  }

  if (val.type === 'tuple') {
    // Keep tuple order but normalize each element
    const normalized = val.value.map(normalizeValue);
    return '(' + normalized.join(', ') + ')';
  }

  if (val.type === 'dictionary') {
    // Sort keys for consistent comparison
    const keys = Object.keys(val.value).sort();
    const pairs = keys.map(k => `${k} = ${normalizeValue(val.value[k])}`);
    return '{ ' + pairs.join(', ') + ' }';
  }

  if (val.type === 'asset') {
    return '@' + val.value + '@';
  }

  if (val.type === 'path') {
    return val.value;
  }

  if (val.type === 'timeSamples') {
    // Sort time codes numerically for consistent comparison
    const timeCodes = Object.keys(val.value).sort((a, b) => parseFloat(a) - parseFloat(b));
    const pairs = timeCodes.map(tc => `${tc}: ${normalizeValue(val.value[tc])}`);
    return '{ ' + pairs.join(', ') + ' }';
  }

  return JSON.stringify(val);
}

/**
 * Resolve the filesystem portion of an asset identifier for semantic compare.
 * This is deliberately opt-in: authored asset identifiers remain exact in the
 * general comparator, while flatten-oracle tests may compare their resolved
 * targets. Package-relative suffixes (`outer.usdz[inner.usda]`) are retained.
 */
function resolveAssetPathForCompare(assetPath, baseDir) {
  if (!baseDir || typeof assetPath !== 'string' || assetPath.length === 0) {
    return assetPath;
  }

  let outer = assetPath;
  let packageSuffix = '';
  const packageStart = outer.indexOf('[');
  if (packageStart > 0 && outer.endsWith(']')) {
    packageSuffix = outer.slice(packageStart);
    outer = outer.slice(0, packageStart);
  }

  // Resolver schemes and anonymous identifiers are not filesystem paths.
  // A Windows drive prefix is an absolute filesystem path, not a URI scheme.
  const windowsAbsolute = /^[A-Za-z]:[\\/]/.test(outer);
  if (!windowsAbsolute && /^[A-Za-z][A-Za-z0-9+.-]*:/.test(outer)) {
    return assetPath;
  }

  let resolved;
  if (windowsAbsolute) {
    resolved = path.win32.normalize(outer).replace(/\\/g, '/');
  } else {
    resolved = path.resolve(baseDir, outer);
  }
  return resolved + packageSuffix;
}

// Same formatting rules as normalizeValue(), but canonicalize parsed asset
// values against the per-output source-layer anchor.
function normalizeValueForCompare(val, options, side) {
  if (!options || !options.resolveAssetPaths) return normalizeValue(val);
  if (val && typeof val === 'object' && val.value !== undefined && val.line !== undefined) {
    val = val.value;
  }
  if (val === null || val === undefined || typeof val !== 'object') {
    return normalizeValue(val);
  }
  const baseDir = side === 1 ? options.assetPathBase1 : options.assetPathBase2;
  if (val.type === 'asset') {
    return '@' + resolveAssetPathForCompare(val.value, baseDir) + '@';
  }
  if (val.type === 'array') {
    return '[' + val.value.map(v => normalizeValueForCompare(v, options, side)).join(', ') + ']';
  }
  if (val.type === 'tuple') {
    return '(' + val.value.map(v => normalizeValueForCompare(v, options, side)).join(', ') + ')';
  }
  if (val.type === 'dictionary') {
    const keys = Object.keys(val.value).sort();
    return '{ ' + keys.map(k =>
      `${k} = ${normalizeValueForCompare(val.value[k], options, side)}`).join(', ') + ' }';
  }
  if (val.type === 'timeSamples') {
    const timeCodes = Object.keys(val.value).sort((a, b) => parseFloat(a) - parseFloat(b));
    return '{ ' + timeCodes.map(tc =>
      `${tc}: ${normalizeValueForCompare(val.value[tc], options, side)}`).join(', ') + ' }';
  }
  return normalizeValue(val);
}

/**
 * Check if a normalized value is numeric
 */
function isNumericValue(normalizedValue) {
  if (typeof normalizedValue !== 'string') return false;
  const num = parseFloat(normalizedValue);
  return !isNaN(num) && isFinite(num);
}

/**
 * Compare two numeric values with epsilon tolerance
 */
function areNumbersEqual(val1, val2, epsilon = 1e-6) {
  const num1 = parseFloat(val1);
  const num2 = parseFloat(val2);

  // Handle special cases
  if (isNaN(num1) || isNaN(num2)) return false;
  if (!isFinite(num1) || !isFinite(num2)) return num1 === num2;

  // Use relative epsilon for large numbers, absolute for small numbers
  const absNum1 = Math.abs(num1);
  const absNum2 = Math.abs(num2);
  const maxAbs = Math.max(absNum1, absNum2);

  if (maxAbs < 1.0) {
    // For small numbers, use absolute epsilon
    return Math.abs(num1 - num2) <= epsilon;
  } else {
    // For larger numbers, use relative epsilon
    return Math.abs(num1 - num2) <= epsilon * maxAbs;
  }
}

/**
 * Compare two timeSamples values with epsilon tolerance for numeric values
 * @param {Object} val1 - First timeSamples object
 * @param {Object} val2 - Second timeSamples object
 * @param {boolean|number} epsilonOrIsHalf - Either epsilon tolerance (number) or isHalf flag (boolean)
 */
function areTimeSamplesEqual(val1, val2, epsilonOrIsHalf = 1e-6, options = {}) {
  // Check if both are timeSamples objects
  if (!val1 || !val2 || val1.type !== 'timeSamples' || val2.type !== 'timeSamples') {
    return false;
  }

  // Determine if we're using half-precision comparison
  const isHalf = typeof epsilonOrIsHalf === 'boolean' ? epsilonOrIsHalf : false;
  const epsilon = typeof epsilonOrIsHalf === 'number' ? epsilonOrIsHalf : 1e-6;

  const times1 = Object.keys(val1.value);
  const times2 = Object.keys(val2.value);

  // Check if they have the same number of time samples
  if (times1.length !== times2.length) {
    return false;
  }

  // Sort time codes for consistent comparison
  times1.sort((a, b) => parseFloat(a) - parseFloat(b));
  times2.sort((a, b) => parseFloat(a) - parseFloat(b));

  // Compare each time code and its value
  for (let i = 0; i < times1.length; i++) {
    const t1 = times1[i];
    const t2 = times2[i];

    // Compare time codes
    if (!areNumbersEqual(t1, t2, epsilon)) {
      return false;
    }

    // Get raw values for half-precision comparison
    const rawV1 = val1.value[t1];
    const rawV2 = val2.value[t2];

    if (isHalf) {
      // Use half-precision binary comparison
      if (!areValuesEqualAsHalf(rawV1, rawV2)) {
        return false;
      }
    } else {
      // Compare values at this time
      const v1 = normalizeValueForCompare(rawV1, options, 1);
      const v2 = normalizeValueForCompare(rawV2, options, 2);

      // Use epsilon comparison for numeric values
      if (isNumericValue(v1) && isNumericValue(v2)) {
        if (!areNumbersEqual(v1, v2, epsilon)) {
          return false;
        }
      } else {
        // Non-numeric values must match exactly
        if (v1 !== v2) {
          return false;
        }
      }
    }
  }

  return true;
}

/**
 * Compare two USDA structures
 */
function compareUsda(usda1, usda2, options = {}) {
  const differences = [];
  const { verbose = false } = options;

  // Compare metadata
  const metaDiff = compareObjects(usda1.metadata, usda2.metadata, 'metadata', options);
  differences.push(...metaDiff);

  // Build prim maps by path
  const primMap1 = buildPrimMap(usda1.prims);
  const primMap2 = buildPrimMap(usda2.prims);

  const allPaths = new Set([...Object.keys(primMap1), ...Object.keys(primMap2)]);

  for (const path of allPaths) {
    const prim1 = primMap1[path];
    const prim2 = primMap2[path];

    if (!prim1) {
      differences.push({
        type: 'prim_missing',
        location: 'file1',
        path,
        line2: prim2?.startLine,
        message: `Prim "${path}" exists in file2 but not in file1`
      });
      continue;
    }

    if (!prim2) {
      differences.push({
        type: 'prim_missing',
        location: 'file2',
        path,
        line1: prim1?.startLine,
        message: `Prim "${path}" exists in file1 but not in file2`
      });
      continue;
    }

    // Compare prim specifier
    if (prim1.specifier !== prim2.specifier) {
      differences.push({
        type: 'specifier_mismatch',
        path,
        line1: prim1.startLine,
        line2: prim2.startLine,
        file1: prim1.specifier,
        file2: prim2.specifier,
        message: `Specifier mismatch at "${path}": "${prim1.specifier}" vs "${prim2.specifier}"`
      });
    }

    // Compare prim type
    if (prim1.typeName !== prim2.typeName) {
      differences.push({
        type: 'type_mismatch',
        path,
        line1: prim1.startLine,
        line2: prim2.startLine,
        file1: prim1.typeName,
        file2: prim2.typeName,
        message: `Type mismatch at "${path}": "${prim1.typeName}" vs "${prim2.typeName}"`
      });
    }

    // Compare attributes
    const attrDiff = compareAttributes(prim1.attributes, prim2.attributes, path, options);
    differences.push(...attrDiff);

    // Compare relationships (normalize listop qualifiers)
    const relDiff = compareRelationships(prim1.relationships, prim2.relationships, path, options);
    differences.push(...relDiff);

    // Compare prim metadata
    const primMetaDiff = compareObjects(prim1.metadata, prim2.metadata, `${path} metadata`, options);
    differences.push(...primMetaDiff);
  }

  return differences;
}

/**
 * Build a flat map of prims by path
 */
function buildPrimMap(prims, map = {}) {
  for (const prim of prims) {
    map[prim.path] = prim;
    buildPrimMap(prim.children, map);
  }
  return map;
}

/**
 * Extract just the attribute name without type annotation
 * E.g., "texCoord2f inputs:st.connect" -> "inputs:st.connect"
 */
function extractAttributeName(fullKey) {
  // Split by spaces and take the last part(s) as the attribute name
  const parts = fullKey.trim().split(/\s+/);

  if (parts.length === 0) return '';

  // Skip known prefixes: uniform, custom, varying, config, prepend, append, delete, add, reorder
  const prefixes = ['uniform', 'custom', 'varying', 'config', 'prepend', 'append', 'delete', 'add', 'reorder'];
  let i = 0;

  while (i < parts.length && prefixes.includes(parts[i])) {
    i++;
  }

  // Skip the type annotation (everything until we find the attribute name with namespace or dot)
  // Type annotations are typically: float, double, int, bool, token, string, asset, etc.
  // They may have [] for arrays or be complex types like texCoord2f, normal3f, etc.
  if (i < parts.length) {
    // Check if next part looks like a type (contains only letters, numbers, [] or is a role type)
    const possibleType = parts[i];
    // Type if it:
    // 1. Contains only alphanumeric, brackets, but no colons or dots (not an attribute name)
    // 2. Doesn't already contain a colon (which indicates namespace like "inputs:file")
    if (possibleType.match(/^[a-zA-Z0-9\[\]]+$/) && !possibleType.includes(':') && !possibleType.includes('.')) {
      i++;
    }
  }

  // Return the remaining parts joined as the attribute name
  const result = parts.slice(i).join(' ').trim();
  return result;
}

// Only bool declarations allow numeric and textual boolean spellings to compare
// interchangeably. Do not coerce strings, tokens, or arbitrary numeric values.
function normalizeBooleanValue(value) {
  if (!value || typeof value !== 'object') return value;
  if ((value.type === 'token' || value.type === 'keyword') &&
      /^(true|false)$/i.test(value.value)) {
    return { type: 'token', value: value.value.toLowerCase() };
  }
  if (value.type === 'number' && (Number(value.value) === 0 || Number(value.value) === 1)) {
    return { type: 'token', value: Number(value.value) === 1 ? 'true' : 'false' };
  }
  if (value.type === 'array') {
    return { ...value, value: value.value.map(normalizeBooleanValue) };
  }
  if (value.type === 'timeSamples') {
    return { ...value, value: Object.fromEntries(Object.entries(value.value)
      .map(([time, sample]) => [time, normalizeBooleanValue(sample)])) };
  }
  return value;
}

/** Compare attributes between two prims. */
function compareAttributes(attrs1, attrs2, primPath, options = {}) {
  const differences = [];

  // Create a map of normalized attribute names to their original keys
  // This makes comparison order-independent
  const attrMap1 = {};
  const attrMap2 = {};

  for (const key of Object.keys(attrs1)) {
    const normName = extractAttributeName(key);
    // Skip empty attribute names (parser edge cases)
    if (normName && normName.length > 0) {
      attrMap1[normName] = key;
    }
  }

  for (const key of Object.keys(attrs2)) {
    const normName = extractAttributeName(key);
    // Skip empty attribute names (parser edge cases)
    if (normName && normName.length > 0) {
      attrMap2[normName] = key;
    }
  }

  // Compare all attributes (order-independent due to using Set of normalized names)
  const allNormNames = new Set([...Object.keys(attrMap1), ...Object.keys(attrMap2)]);

  for (const normName of allNormNames) {
    const key1 = attrMap1[normName];
    const key2 = attrMap2[normName];

    const val1 = key1 ? attrs1[key1] : undefined;
    const val2 = key2 ? attrs2[key2] : undefined;

    // Extract location info if present
    const line1 = val1 && typeof val1 === 'object' && val1.line ? val1.line : undefined;
    const line2 = val2 && typeof val2 === 'object' && val2.line ? val2.line : undefined;

    if (val1 === undefined) {
      differences.push({
        type: 'attribute_missing',
        location: 'file1',
        path: primPath,
        attribute: normName,
        line2: line2,
        message: `Attribute "${normName}" at "${primPath}" exists in file2 but not in file1`
      });
      continue;
    }

    if (val2 === undefined) {
      differences.push({
        type: 'attribute_missing',
        location: 'file2',
        path: primPath,
        attribute: normName,
        line1: line1,
        message: `Attribute "${normName}" at "${primPath}" exists in file1 but not in file2`
      });
      continue;
    }

    // Extract actual values if wrapped with location info
    let actualVal1 = val1 && typeof val1 === 'object' && val1.value !== undefined && val1.line !== undefined ? val1.value : val1;
    let actualVal2 = val2 && typeof val2 === 'object' && val2.value !== undefined && val2.line !== undefined ? val2.value : val2;

    const boolType1 = /^(?:(?:custom|uniform|varying)\s+)*bool(\[\])?\s+/.exec(key1);
    const boolType2 = /^(?:(?:custom|uniform|varying)\s+)*bool(\[\])?\s+/.exec(key2);
    if (boolType1 && boolType2 && boolType1[1] === boolType2[1]) {
      actualVal1 = normalizeBooleanValue(actualVal1);
      actualVal2 = normalizeBooleanValue(actualVal2);
    }

    // Check if this is a half-precision attribute (using original keys which have type info)
    const isHalfAttr = isHalfPrecisionAttribute(key1 || '') || isHalfPrecisionAttribute(key2 || '');

    // Check for timeSamples before normalization
    let valuesEqual = false;

    // Special handling for timeSamples (compare before normalization)
    if (actualVal1 && typeof actualVal1 === 'object' && actualVal1.type === 'timeSamples' &&
        actualVal2 && typeof actualVal2 === 'object' && actualVal2.type === 'timeSamples') {
      valuesEqual = areTimeSamplesEqual(actualVal1, actualVal2, isHalfAttr, options);
    } else if (isHalfAttr) {
      // Half-precision comparison: compare as binary representation
      valuesEqual = areValuesEqualAsHalf(actualVal1, actualVal2);
    } else {
      // Normal comparison path
      const norm1 = normalizeValueForCompare(actualVal1, options, 1);
      const norm2 = normalizeValueForCompare(actualVal2, options, 2);

      // Use epsilon comparison for numeric values
      if (isNumericValue(norm1) && isNumericValue(norm2)) {
        valuesEqual = areNumbersEqual(norm1, norm2);
      } else {
        valuesEqual = (norm1 === norm2);
      }
    }

    if (!valuesEqual) {
      // Normalize for error message display
      const norm1 = normalizeValue(val1);
      const norm2 = normalizeValue(val2);
      differences.push({
        type: 'attribute_value_mismatch',
        path: primPath,
        attribute: normName,
        line1: line1,
        line2: line2,
        file1: norm1,
        file2: norm2,
        message: `Attribute "${normName}" at "${primPath}" differs: "${truncate(norm1, 50)}" vs "${truncate(norm2, 50)}"`
      });
    }
  }

  return differences;
}

/**
 * Normalize relationship name by stripping qualifiers (listops and custom)
 * E.g., "delete myheight" -> "myheight", "append custom myval" -> "myval"
 */
function normalizeRelationshipName(name) {
  const qualifierPrefixes = ['delete ', 'prepend ', 'append ', 'add ', 'reorder ', 'custom '];
  let normalized = name;
  let changed = true;
  // Keep stripping qualifiers until no more are found
  while (changed) {
    changed = false;
    for (const prefix of qualifierPrefixes) {
      if (normalized.startsWith(prefix)) {
        normalized = normalized.slice(prefix.length);
        changed = true;
        break;
      }
    }
  }
  return normalized;
}

/**
 * Compare relationships between two prims
 * Normalizes listop qualifiers (delete/prepend/append/add/reorder) for comparison
 */
function compareRelationships(rels1, rels2, primPath, options = {}) {
  const differences = [];

  // Create maps of normalized names to original keys
  const relMap1 = {};
  const relMap2 = {};

  for (const key of Object.keys(rels1)) {
    const normName = normalizeRelationshipName(key);
    relMap1[normName] = key;
  }

  for (const key of Object.keys(rels2)) {
    const normName = normalizeRelationshipName(key);
    relMap2[normName] = key;
  }

  const allNormNames = new Set([...Object.keys(relMap1), ...Object.keys(relMap2)]);

  for (const normName of allNormNames) {
    const key1 = relMap1[normName];
    const key2 = relMap2[normName];

    const val1 = key1 ? rels1[key1] : undefined;
    const val2 = key2 ? rels2[key2] : undefined;

    if (val1 === undefined) {
      differences.push({
        type: 'relationship_missing',
        location: 'file1',
        path: primPath,
        relationship: normName,
        message: `Relationship "${normName}" at "${primPath}" exists in file2 but not in file1`
      });
      continue;
    }

    if (val2 === undefined) {
      differences.push({
        type: 'relationship_missing',
        location: 'file2',
        path: primPath,
        relationship: normName,
        message: `Relationship "${normName}" at "${primPath}" exists in file1 but not in file2`
      });
      continue;
    }

    // Compare values (both null means declaration-only, which is equivalent)
    if (val1 === null && val2 === null) {
      continue; // Both are declaration-only, equivalent
    }

    const norm1 = normalizeValueForCompare(val1, options, 1);
    const norm2 = normalizeValueForCompare(val2, options, 2);

    if (norm1 !== norm2) {
      differences.push({
        type: 'relationship_value_mismatch',
        path: primPath,
        relationship: normName,
        file1: norm1,
        file2: norm2,
        message: `Relationship "${normName}" at "${primPath}" differs: "${truncate(norm1, 50)}" vs "${truncate(norm2, 50)}"`
      });
    }
  }

  return differences;
}

/**
 * Compare two objects
 */
function compareObjects(obj1, obj2, context, options = {}) {
  const differences = [];
  const allKeys = new Set([...Object.keys(obj1), ...Object.keys(obj2)]);

  for (const key of allKeys) {
    const val1 = obj1[key];
    const val2 = obj2[key];

    if (val1 === undefined) {
      differences.push({
        type: 'key_missing',
        location: 'file1',
        context,
        key,
        message: `Key "${key}" in ${context} exists in file2 but not in file1`
      });
      continue;
    }

    if (val2 === undefined) {
      differences.push({
        type: 'key_missing',
        location: 'file2',
        context,
        key,
        message: `Key "${key}" in ${context} exists in file1 but not in file2`
      });
      continue;
    }

    // Extract actual values if wrapped with location info
    let actualVal1 = val1 && typeof val1 === 'object' && val1.value !== undefined && val1.line !== undefined ? val1.value : val1;
    let actualVal2 = val2 && typeof val2 === 'object' && val2.value !== undefined && val2.line !== undefined ? val2.value : val2;

    // Check for timeSamples before normalization
    let valuesEqual = false;

    // Special handling for timeSamples (compare before normalization)
    if (actualVal1 && typeof actualVal1 === 'object' && actualVal1.type === 'timeSamples' &&
        actualVal2 && typeof actualVal2 === 'object' && actualVal2.type === 'timeSamples') {
      valuesEqual = areTimeSamplesEqual(actualVal1, actualVal2, 1e-6, options);
    } else {
      // Normal comparison path
      const norm1 = normalizeValueForCompare(val1, options, 1);
      const norm2 = normalizeValueForCompare(val2, options, 2);

      // Use epsilon comparison for numeric values
      if (isNumericValue(norm1) && isNumericValue(norm2)) {
        valuesEqual = areNumbersEqual(norm1, norm2);
      } else {
        valuesEqual = (norm1 === norm2);
      }
    }

    if (!valuesEqual) {
      // Normalize for error message display
      const norm1 = normalizeValue(val1);
      const norm2 = normalizeValue(val2);
      differences.push({
        type: 'value_mismatch',
        context,
        key,
        file1: norm1,
        file2: norm2,
        message: `Value for "${key}" in ${context} differs: "${truncate(norm1, 50)}" vs "${truncate(norm2, 50)}"`
      });
    }
  }

  return differences;
}

/**
 * Truncate string for display
 */
function truncate(str, maxLen) {
  if (str.length <= maxLen) return str;
  return str.slice(0, maxLen - 3) + '...';
}

/**
 * Parse USDA content
 */
function parseUsda(content) {
  const lexer = new UsdaLexer(content);
  const tokens = lexer.tokenize();
  const parser = new UsdaParser(tokens);
  return parser.parse();
}

/**
 * Get a specific line from content
 */
function getLine(content, lineNum) {
  const lines = content.split('\n');
  return lines[lineNum - 1] || '';
}

/**
 * Get context lines around a specific line
 */
function getLineContext(content, lineNum, contextLines = 1) {
  const lines = content.split('\n');
  const start = Math.max(0, lineNum - contextLines - 1);
  const end = Math.min(lines.length, lineNum + contextLines);
  const context = [];

  for (let i = start; i < end; i++) {
    const isTargetLine = (i + 1) === lineNum;
    const marker = isTargetLine ? '>>> ' : '    ';
    context.push(`${marker}${String(i + 1).padStart(5)}: ${lines[i]}`);
  }

  return context.join('\n');
}

/**
 * Format a detailed diff for a single difference
 */
function formatDetailedDiff(diff, content1, content2) {
  let output = `\n  ✗ ${diff.message}\n`;

  if (diff.type === 'attribute_value_mismatch' || diff.type === 'attribute_missing') {
    if (diff.line1) {
      output += `\n    File 1 (line ${diff.line1}):\n`;
      output += getLineContext(content1, diff.line1, 1).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
    if (diff.line2) {
      output += `\n    File 2 (line ${diff.line2}):\n`;
      output += getLineContext(content2, diff.line2, 1).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
  } else if (diff.type === 'prim_missing') {
    if (diff.line1) {
      output += `\n    Prim defined in File 1 at line ${diff.line1}:\n`;
      output += getLineContext(content1, diff.line1, 1).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
    if (diff.line2) {
      output += `\n    Prim defined in File 2 at line ${diff.line2}:\n`;
      output += getLineContext(content2, diff.line2, 1).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
  } else if (diff.type === 'specifier_mismatch' || diff.type === 'type_mismatch') {
    if (diff.line1) {
      output += `\n    File 1 (line ${diff.line1}): ${diff.file1}\n`;
      output += getLineContext(content1, diff.line1, 0).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
    if (diff.line2) {
      output += `\n    File 2 (line ${diff.line2}): ${diff.file2}\n`;
      output += getLineContext(content2, diff.line2, 0).split('\n').map(l => '    ' + l).join('\n');
      output += '\n';
    }
  }

  return output;
}

/**
 * Print usage information
 */
function printUsage() {
  console.log(`
Usage: compare-usda.js [options] <file1.usda> <file2.usda>
       compare-usda.js --lusdcat <path> --usdcat <path> <input.usd>
       compare-usda.js --lusdcat <path> --usdcat <path> "**/*.usd"

Compare USDA outputs at the Prim and Attribute level, ignoring ordering differences.

Supports glob patterns for batch comparison:
  *        - matches any characters except /
  **       - matches any characters including /
  ?        - matches single character
  [abc]    - matches any character in brackets
  {a,b,c}  - matches any of the alternatives

Examples:
  compare-usda.js file1.usda file2.usda
  compare-usda.js --lusdcat ./lusdcat --usdcat usdcat model.usd
  compare-usda.js --lusdcat ./lusdcat --usdcat usdcat "models/*.usda"
  compare-usda.js --lusdcat ./lusdcat --usdcat usdcat "**/*.usd{a,c,z}"
  compare-usda.js --lusdcat ./lusdcat --usdcat usdcat --base-dir /path/to/models "**/*.usd"

Options:
  -h, --help              Show this help message
  -v, --verbose           Show detailed output
  -q, --quiet             Only show if files differ (exit code)
  --lusdcat <path>        Path to lusdcat executable
  --usdcat <path>         Path to usdcat executable
  --base-dir <path>       Base directory for glob patterns (default: current dir)
  --summary               Show summary statistics only
  --detailed-diff         Show detailed diffs with line numbers and context
  --ignore-metadata       Ignore top-level metadata differences
  --ignore-types          Ignore attribute type differences
  --resolve-asset-paths   Compare filesystem asset paths after lexical resolution
  --asset-base1 <path>    Source-layer anchor for file/tool output 1
  --asset-base2 <path>    Source-layer anchor for file/tool output 2
  --float-tolerance <n>   Tolerance for floating point comparison (default: 1e-6)
  --continue-on-error     Continue processing other files if one fails
  --json                  Output results as JSON
  --timeout <ms>          Timeout per file in milliseconds (default: 60000)

Exit codes:
  0 - All files are equivalent
  1 - At least one file differs
  2 - Error occurred
`);
}

/**
 * Compare a single file with lusdcat vs usdcat
 */
function compareSingleFile(inputFile, options) {
  const result = {
    file: inputFile,
    status: 'unknown',
    differences: [],
    error: null,
    expectedDifference: null,
    content1: null,
    content2: null
  };

  // Filename-based XFAIL for .usdc fixtures that can't carry an inline
  // marker. Keyed on basename so it's path-independent. Mirrors the
  // `# XFAIL:` comment used by .usda fixtures.
  const usdcXfails = {
    'utf8-assetpath-001.usdc':
      'pixar-usdcat-parse — pxr <= 23.08 rejects UTF-8 in asset paths ' +
      '(fixed in 24.x per OpenUSD#2560)',
    'refs-customdata-001.usdc':
      'reference customData has no slot in the next arc model (arc itself ' +
      'is preserved; dict is skipped by design)',
    'delete-apischemas-001.usdc':
      'delete-apiSchemas sublists apply as in-place removals; the writer ' +
      'does not re-emit a delete qualifier (known crate-writer P1)',
    'rel-inherits-none-001.usdc':
      'legacy lusdcat prints explicit-empty inherits as [] where pxr keeps ' +
      'None (legacy-side gap; the next reader preserves the clear)',
  };
  const base = require('path').basename(inputFile);
  if (usdcXfails[base]) {
    result.status = 'xfail';
    result.error = usdcXfails[base];
    return result;
  }

  // Check for XFAIL/XDIFF marker near the top of USDA fixtures. XFAIL skips
  // comparison because one tool is expected not to parse the file. XDIFF still
  // compares both outputs, but classifies non-empty diffs as expected.
  let xdiffTag = null;
  try {
    const head = fs.readFileSync(inputFile, 'utf-8').split('\n').slice(0, 32);
    for (const line of head) {
      const trimmed = line.trim();
      if (trimmed.startsWith('# XFAIL:')) {
        const tag = trimmed.slice('# XFAIL:'.length).trim();
        result.status = 'xfail';
        result.error = tag;
        return result;
      }
      if (trimmed.startsWith('# XDIFF:')) {
        xdiffTag = trimmed.slice('# XDIFF:'.length).trim();
      }
    }
  } catch (e) { /* ignore read errors, will fail later */ }
  result.expectedDifference = xdiffTag;

  try {
    let content1, content2;

    // Run lusdcat with timeout
    const runUsdTool = (exe, filePath) => {
      return execFileSync(exe, [filePath], {
        encoding: 'utf-8',
        maxBuffer: 100 * 1024 * 1024,
        stdio: ['pipe', 'pipe', 'pipe'],
        timeout: options.timeout
      });
    };

    // Run lusdcat with timeout
    try {
      content1 = runUsdTool(options.lusdcat, inputFile);
    } catch (e) {
      result.status = 'error';
      const timeoutMsg = e.code === 'ETIMEDOUT' ? `timeout (${options.timeout / 1000}s)` : e.message;
      result.error = `lusdcat failed: ${timeoutMsg}`;
      return result;
    }

    // Run usdcat with timeout
    try {
      content2 = runUsdTool(options.usdcat, inputFile);
    } catch (e) {
      result.status = 'error';
      const timeoutMsg = e.code === 'ETIMEDOUT' ? `timeout (${options.timeout / 1000}s)` : e.message;
      result.error = `usdcat failed: ${timeoutMsg}`;
      return result;
    }

    // Store contents for detailed diff reporting
    result.content1 = content1;
    result.content2 = content2;

    // Parse both USDA contents
    let usda1, usda2;
    try {
      usda1 = parseUsda(content1);
    } catch (e) {
      result.status = 'error';
      result.error = `Failed to parse lusdcat output: ${e.message}`;
      // Show context in verbose mode
      if (options.verbose) {
        const lines = content1.split('\n');
        const errorLine = parseInt(e.message.match(/position (\d+)/) ? RegExp.$1 : 0);
        if (errorLine > 0) {
          const charPos = errorLine;
          let lineNum = 0;
          let pos = 0;
          for (let i = 0; i < lines.length; i++) {
            pos += lines[i].length + 1;
            lineNum = i;
            if (pos >= charPos) break;
          }
          result.error += `\n    at line ${lineNum + 1}: ${lines[lineNum]}`;
        }
      }
      return result;
    }
    try {
      usda2 = parseUsda(content2);
    } catch (e) {
      result.status = 'error';
      result.error = `Failed to parse usdcat output: ${e.message}`;
      // Show context in verbose mode
      if (options.verbose) {
        const lines = content2.split('\n');
        const errorLine = parseInt(e.message.match(/position (\d+)/) ? RegExp.$1 : 0);
        if (errorLine > 0) {
          const charPos = errorLine;
          let lineNum = 0;
          let pos = 0;
          for (let i = 0; i < lines.length; i++) {
            pos += lines[i].length + 1;
            lineNum = i;
            if (pos >= charPos) break;
          }
          result.error += `\n    at line ${lineNum + 1}: ${lines[lineNum]}`;
        }
      }
      return result;
    }

    // Compare. Tool outputs both originate from this input layer, so relative
    // assets share its directory unless the caller supplied explicit anchors.
    const compareOptions = { ...options };
    if (compareOptions.resolveAssetPaths) {
      const inputBase = path.dirname(path.resolve(inputFile));
      compareOptions.assetPathBase1 ||= inputBase;
      compareOptions.assetPathBase2 ||= inputBase;
    }
    let differences = compareUsda(usda1, usda2, compareOptions);

    // Filter differences based on options
    if (options.ignoreMetadata) {
      differences = differences.filter(d => !d.context?.includes('metadata'));
    }

    if (options.ignoreTypes) {
      differences = differences.filter(d => d.type !== 'type_mismatch');
    }

    result.differences = differences;
    result.status = differences.length === 0
      ? 'equivalent'
      : (xdiffTag ? 'xdiff' : 'different');

  } catch (error) {
    result.status = 'error';
    result.error = error.message;
  }

  return result;
}

/**
 * Main entry point
 */
function main() {
  const args = process.argv.slice(2);

  if (args.length === 0 || args.includes('-h') || args.includes('--help')) {
    printUsage();
    process.exit(0);
  }

  const options = {
    verbose: false,
    quiet: false,
    summary: false,
    detailedDiff: false,
    ignoreMetadata: false,
    ignoreTypes: false,
    resolveAssetPaths: false,
    assetPathBase1: null,
    assetPathBase2: null,
    floatTolerance: 1e-6,
    lusdcat: null,
    usdcat: null,
    baseDir: process.cwd(),
    continueOnError: false,
    json: false,
    timeout: 60000,
    printCommands: false,
    files: []
  };

  // Parse arguments
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    switch (arg) {
      case '-v':
      case '--verbose':
        options.verbose = true;
        break;
      case '-q':
      case '--quiet':
        options.quiet = true;
        break;
      case '--summary':
        options.summary = true;
        break;
      case '--detailed-diff':
        options.detailedDiff = true;
        break;
      case '--ignore-metadata':
        options.ignoreMetadata = true;
        break;
      case '--ignore-types':
        options.ignoreTypes = true;
        break;
      case '--resolve-asset-paths':
        options.resolveAssetPaths = true;
        break;
      case '--asset-base1':
        options.assetPathBase1 = path.resolve(args[++i]);
        break;
      case '--asset-base2':
        options.assetPathBase2 = path.resolve(args[++i]);
        break;
      case '--float-tolerance':
        options.floatTolerance = parseFloat(args[++i]);
        break;
      case '--lusdcat':
        options.lusdcat = args[++i];
        break;
      case '--usdcat':
        options.usdcat = args[++i];
        break;
      case '--base-dir':
        options.baseDir = args[++i];
        break;
      case '--continue-on-error':
        options.continueOnError = true;
        break;
      case '--json':
        options.json = true;
        break;
      case '--timeout':
        options.timeout = parseInt(args[++i], 10);
        break;
      case '--print-commands':
        options.printCommands = true;
        break;
      default:
        if (!arg.startsWith('-')) {
          options.files.push(arg);
        }
        break;
    }
  }

  try {
    // Mode 1: Compare two USDA files directly
    if (options.files.length === 2 && !options.lusdcat && !options.usdcat) {
      const label1 = options.files[0];
      const label2 = options.files[1];
      const content1 = fs.readFileSync(options.files[0], 'utf-8');
      const content2 = fs.readFileSync(options.files[1], 'utf-8');

      // Check for VALUE_PPRINT placeholder (LightUSD pprinter bug)
      const vprintWarnings = [];
      for (const [content, label] of [[content1, label1], [content2, label2]]) {
        const vpLines = content.split('\n')
          .map((line, i) => ({ line: line.trim(), num: i + 1 }))
          .filter(({ line }) => line.includes('VALUE_PPRINT'));
        for (const { line, num } of vpLines) {
          vprintWarnings.push({ file: label, lineNum: num, text: line });
        }
      }
      if (vprintWarnings.length > 0) {
        console.error(`\nERROR: VALUE_PPRINT placeholder detected (LightUSD pprinter bug):`);
        for (const w of vprintWarnings) {
          console.error(`  ${w.file}:${w.lineNum}: ${w.text.substring(0, 120)}`);
        }
        console.error('');
        process.exit(2);
      }

      // Parse both USDA contents
      if (!options.quiet && !options.json) {
        console.log('Parsing USDA files...');
      }

      const usda1 = parseUsda(content1);
      const usda2 = parseUsda(content2);

      if (options.resolveAssetPaths) {
        options.assetPathBase1 ||= path.dirname(path.resolve(options.files[0]));
        options.assetPathBase2 ||= path.dirname(path.resolve(options.files[1]));
      }

      // Compare
      if (!options.quiet && !options.json) {
        console.log('Comparing...\n');
      }

      let differences = compareUsda(usda1, usda2, options);

      // Filter differences based on options
      if (options.ignoreMetadata) {
        differences = differences.filter(d => !d.context?.includes('metadata'));
      }

      if (options.ignoreTypes) {
        differences = differences.filter(d => d.type !== 'type_mismatch');
      }

      if (options.json) {
        console.log(JSON.stringify({
          file1: label1,
          file2: label2,
          status: differences.length === 0 ? 'equivalent' : 'different',
          differenceCount: differences.length,
          differences: options.summary ? undefined : differences
        }, null, 2));
        process.exit(differences.length === 0 ? 0 : 1);
      }

      // Output results
      if (differences.length === 0) {
        if (!options.quiet) {
          console.log('✓ Files are equivalent (attributes and prims match)');
        }
        process.exit(0);
      } else {
        if (!options.quiet) {
          console.log(`✗ Found ${differences.length} difference(s):\n`);

          if (options.summary) {
            // Group by type
            const byType = {};
            for (const diff of differences) {
              byType[diff.type] = (byType[diff.type] || 0) + 1;
            }
            console.log('Summary:');
            for (const [type, count] of Object.entries(byType)) {
              console.log(`  ${type}: ${count}`);
            }
          } else if (options.detailedDiff) {
            // Show detailed diffs with line numbers and context
            for (const diff of differences) {
              console.log(formatDetailedDiff(diff, content1, content2));
            }
          } else {
            // Show all differences
            for (const diff of differences) {
              console.log(`  - ${diff.message}`);
              if (options.verbose && diff.file1 && diff.file2) {
                console.log(`      File 1: ${diff.file1}`);
                console.log(`      File 2: ${diff.file2}`);
              }
            }
          }

          console.log(`\nFiles compared: ${label1} vs ${label2}`);
        }
        process.exit(1);
      }
    }
    // Mode 2: Run lusdcat and usdcat on input file(s) - supports glob patterns
    else if (options.files.length >= 1 && options.lusdcat && options.usdcat) {
      // Expand glob patterns
      const expandedFiles = expandFilePatterns(options.files, options.baseDir)
        // Filter out empty strings to prevent invalid file paths
        .filter(f => f && f.trim().length > 0);

      if (expandedFiles.length === 0) {
        console.error('Error: No files found matching the pattern(s).');
        process.exit(2);
      }

      if (!options.quiet && !options.json) {
        console.log(`Found ${expandedFiles.length} file(s) to compare\n`);
      }

      const results = [];
      let equivalent = 0;
      let different = 0;
      let errors = 0;
      let xfails = 0;
      let xdiffs = 0;

      for (let i = 0; i < expandedFiles.length; i++) {
        const inputFile = expandedFiles[i];

        if (!options.quiet && !options.json) {
          console.log(`[${i + 1}/${expandedFiles.length}] Processing: ${inputFile}`);
          if (options.printCommands) {
            console.log(`  $ "${options.lusdcat}" "${inputFile}"`);
            console.log(`  $ "${options.usdcat}" "${inputFile}"`);
          }
        }

        const result = compareSingleFile(inputFile, options);
        results.push(result);

        if (result.status === 'xfail') {
          xfails++;
          if (!options.quiet && !options.json) {
            console.log(`  XFAIL: ${result.error}\n`);
          }
        } else if (result.status === 'xdiff') {
          xdiffs++;
          if (!options.quiet && !options.json) {
            console.log(`  XDIFF: ${result.differences.length} expected difference(s)`);
            if (result.expectedDifference) {
              console.log(`  Reason: ${result.expectedDifference}`);
            }
            if (options.detailedDiff && result.content1 && result.content2) {
              for (const diff of result.differences) {
                console.log(formatDetailedDiff(diff, result.content1, result.content2));
              }
            } else if (options.verbose && !options.summary) {
              for (const diff of result.differences) {
                console.log(`    - ${diff.message}`);
              }
            }
            console.log('');
          }
        } else if (result.status === 'equivalent') {
          equivalent++;
          if (!options.quiet && !options.json) {
            console.log(`  ✓ Equivalent\n`);
          }
        } else if (result.status === 'different') {
          different++;
          if (!options.quiet && !options.json) {
            console.log(`  ✗ ${result.differences.length} difference(s)`);
            if (options.detailedDiff && result.content1 && result.content2) {
              // Show detailed diffs with line numbers and context
              for (const diff of result.differences) {
                console.log(formatDetailedDiff(diff, result.content1, result.content2));
              }
            } else if (options.verbose && !options.summary) {
              for (const diff of result.differences) {
                console.log(`    - ${diff.message}`);
              }
            }
            console.log('');
          }
        } else {
          errors++;
          if (!options.quiet && !options.json) {
            console.log(`  ⚠ Error: ${result.error}\n`);
          }
          if (!options.continueOnError) {
            break;
          }
        }
      }

      // Output final summary
      if (options.json) {
        console.log(JSON.stringify({
          totalFiles: expandedFiles.length,
          equivalent,
          different,
          errors,
          xfails,
          xdiffs,
          results: options.summary ? undefined : results
        }, null, 2));
      } else if (!options.quiet) {
        console.log('═'.repeat(50));
        console.log(`Summary: ${expandedFiles.length} file(s) processed`);
        console.log(`  ✓ Equivalent: ${equivalent}`);
        console.log(`  ✗ Different:  ${different}`);
        if (xfails > 0) {
          console.log(`  XFAIL:        ${xfails}`);
        }
        if (xdiffs > 0) {
          console.log(`  XDIFF:        ${xdiffs}`);
        }
        if (errors > 0) {
          console.log(`  ⚠ Errors:     ${errors}`);
        }
      }

      // Exit code
      if (errors > 0 && !options.continueOnError) {
        process.exit(2);
      } else if (different > 0 || errors > 0) {
        process.exit(1);
      } else {
        process.exit(0);
      }
    }
    else {
      console.error('Error: Invalid arguments. Either provide two USDA files or use --lusdcat and --usdcat with input file(s)/pattern(s).');
      printUsage();
      process.exit(2);
    }

  } catch (error) {
    console.error(`Error: ${error.message}`);
    if (options.verbose) {
      console.error(error.stack);
    }
    process.exit(2);
  }
}

// Export for testing
module.exports = {
  UsdaLexer,
  UsdaParser,
  parseUsda,
  compareUsda,
  normalizeValue,
  normalizeValueForCompare,
  resolveAssetPathForCompare,
  expandGlob,
  expandFilePatterns,
  isGlobPattern,
  globToRegex,
  // Half-precision (fp16) utilities
  floatToHalfBits,
  halfBitsToFloat,
  areHalfValuesEqual,
  areValuesEqualAsHalf,
  isHalfPrecisionAttribute
};

// Run if executed directly
if (require.main === module) {
  main();
}
