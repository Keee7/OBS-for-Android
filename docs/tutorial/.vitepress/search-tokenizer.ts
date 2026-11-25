const CJK_RUN_RE = /[\u4e00-\u9fff]+/g
const WORD_RUN_RE = /[a-z0-9]+/gi

/**
 * Bilingual tokenizer for VitePress MiniSearch:
 * - Splits ASCII/Latin/numeric sequences into lowercased word tokens.
 * - Splits Chinese (Han) runs into overlapping 2-character bigrams so any
 *   2+ character Chinese query matches sub-phrases reliably.
 */
export function searchTokenize(text: string): string[] {
  if (!text) return []
  const tokens: string[] = []

  for (const m of text.matchAll(WORD_RUN_RE)) {
    tokens.push(m[0].toLowerCase())
  }

  for (const m of text.matchAll(CJK_RUN_RE)) {
    const run = m[0]
    if (run.length === 1) {
      tokens.push(run)
      continue
    }
    for (let i = 0; i < run.length - 1; i++) {
      tokens.push(run.slice(i, i + 2))
    }
  }

  return tokens
}
