// Human-readable summaries over BenchResult rows.
//
// Rules, kept deliberately simple so the numbers on the page can be
// checked by hand:
// - A "cell" is one (problem, input) pair.
// - Within a cell, a series (a language, or one compiler/code variant)
//   is represented by its fastest non-timeout time. A series whose only
//   results in a cell timed out is shown as a timeout, not as a number.
// - A ratio is series time / fastest time in the cell. 1.0x is fastest.
// - The leaderboard uses single-thread results only, excludes timeouts,
//   and only counts cells where at least two languages have a result,
//   so a language cannot win a cell unopposed.
// - The leaderboard score is the geometric mean of a language's ratios.
//   1.0x means fastest in every counted cell; larger means slower.

export type SeriesCell = {
  timeMS: number | null
  ratio: number | null
  timeout: boolean
  missing: boolean
}

export type SeriesRow = {
  key: string
  label: string
  cells: { [input: string]: SeriesCell }
  wins: number
  counted: number
}

export type LeaderboardRow = {
  lang: string
  langDisplay: string
  geomean: number
  wins: number
  cells: number
}

export function isTimeout(b: BenchResult): boolean {
  return b.timeout === true || b.timeMS <= 0
}

export function isVisible(b: BenchResult, showSt: boolean, showMt: boolean): boolean {
  return (!b.par && showSt) || (!!b.par && showMt)
}

export function sortInputs(inputs: string[]): string[] {
  const num = (s: string): number | null => {
    const m = /-?\d+(\.\d+)?/.exec(s)
    return m ? parseFloat(m[0]) : null
  }
  return inputs.slice().sort((a, b) => {
    const na = num(a)
    const nb = num(b)
    if (na !== null && nb !== null && na !== nb) return na - nb
    return a < b ? -1 : a > b ? 1 : 0
  })
}

export function largestInput(inputs: string[]): string {
  const sorted = sortInputs(inputs)
  return sorted[sorted.length - 1]
}

// Best (fastest) non-timeout time per series key, plus timeout/missing
// state, for one cell. `benches` is already filtered to the cell.
export function bestBySeries(
  benches: BenchResult[],
  keyOf: (b: BenchResult) => string,
  labelOf: (key: string) => string
): (SeriesRow & { _best: number | null; _timeout: boolean })[] {
  const byKey: { [key: string]: BenchResult[] } = {}
  benches.forEach((b) => {
    const k = keyOf(b)
    if (!byKey[k]) byKey[k] = []
    byKey[k].push(b)
  })
  return Object.keys(byKey)
    .sort()
    .map((key) => {
      const rows = byKey[key]
      const ok = rows.filter((b) => !isTimeout(b))
      const best = ok.length > 0 ? Math.min.apply(null, ok.map((b) => b.timeMS)) : null
      return {
        key,
        label: labelOf(key),
        cells: {},
        wins: 0,
        counted: 0,
        _best: best,
        _timeout: ok.length === 0 && rows.length > 0,
      } as SeriesRow & { _best: number | null; _timeout: boolean }
    })
}

// Build one matrix for a problem: rows are series, columns are inputs.
// `benches` is already filtered to the problem (and to the visible
// single/parallel selection). Returns rows with a cell per input, with
// ratios filled in against the fastest series in each input column.
export function matrixForProblem(
  benches: BenchResult[],
  inputs: string[],
  keyOf: (b: BenchResult) => string,
  labelOf: (key: string) => string
): SeriesRow[] {
  const sortedInputs = sortInputs(inputs)
  const perInput: { [input: string]: (SeriesRow & { _best: number | null; _timeout: boolean })[] } = {}
  const keys: string[] = []
  sortedInputs.forEach((input) => {
    const rows = bestBySeries(
      benches.filter((b) => b.input === input),
      keyOf,
      labelOf
    )
    perInput[input] = rows
    rows.forEach((r) => {
      if (keys.indexOf(r.key) === -1) keys.push(r.key)
    })
  })
  // Fastest time per input column, over series that have a time.
  const fastest: { [input: string]: number } = {}
  const slowest: { [input: string]: number } = {}
  sortedInputs.forEach((input) => {
    const times = perInput[input]
      .map((r) => r._best)
      .filter((t): t is number => t !== null)
    if (times.length > 0) {
      fastest[input] = Math.min.apply(null, times)
      slowest[input] = Math.max.apply(null, times)
    }
  })
  return keys.sort().map((key) => {
    let wins = 0
    let counted = 0
    const cells: { [input: string]: SeriesCell } = {}
    sortedInputs.forEach((input) => {
      const row = perInput[input].filter((r) => r.key === key)[0]
      if (!row) {
        cells[input] = { timeMS: null, ratio: null, timeout: false, missing: true }
      } else if (row._best === null) {
        cells[input] = { timeMS: null, ratio: null, timeout: row._timeout, missing: false }
      } else {
        const ratio = fastest[input] ? row._best / fastest[input] : 1
        counted += 1
        if (ratio === 1) wins += 1
        cells[input] = { timeMS: row._best, ratio, timeout: false, missing: false }
      }
    })
    return { key, label: labelOf(key), cells, wins, counted }
  })
}

export function matrixSlowest(rows: SeriesRow[], input: string): number | null {
  const times = rows
    .map((r) => r.cells[input])
    .filter((c) => c && c.timeMS !== null)
    .map((c) => c.timeMS as number)
  return times.length > 0 ? Math.max.apply(null, times) : null
}

// Bar width for a time within a column, 0-100. Linear scale is the
// default; log scale keeps the fastest bar visible (min 10%) and is
// only a visual aid, never the number itself.
export function barWidth(
  timeMS: number,
  fastestMS: number,
  slowestMS: number,
  logScale: boolean
): number {
  if (slowestMS <= fastestMS) return 100
  if (!logScale) return (timeMS / slowestMS) * 100
  const lo = Math.log(Math.max(fastestMS, 0.000001))
  const hi = Math.log(Math.max(slowestMS, 0.000001))
  const v = Math.log(Math.max(timeMS, 0.000001))
  return 10 + 90 * ((v - lo) / (hi - lo))
}

export function leaderboard(langs: LangBenchResults[]): LeaderboardRow[] {
  // Group single-thread, non-timeout results by cell.
  const cells: { [cell: string]: { [lang: string]: number } } = {}
  const display: { [lang: string]: string } = {}
  langs.forEach((l) => {
    display[l.lang] = l.langDisplay
    l.benchmarks.forEach((b) => {
      if (b.par || isTimeout(b)) return
      const cell = b.test + '\n' + b.input
      if (!cells[cell]) cells[cell] = {}
      const prev = cells[cell][l.lang]
      if (prev === undefined || b.timeMS < prev) cells[cell][l.lang] = b.timeMS
    })
  })
  const stats: {
    [lang: string]: { logSum: number; wins: number; cells: number }
  } = {}
  Object.keys(cells).forEach((cell) => {
    const byLang = cells[cell]
    const langKeys = Object.keys(byLang)
    if (langKeys.length < 2) return
    const fastest = Math.min.apply(
      null,
      langKeys.map((k) => byLang[k])
    )
    langKeys.forEach((k) => {
      if (!stats[k]) stats[k] = { logSum: 0, wins: 0, cells: 0 }
      stats[k].logSum += Math.log(byLang[k] / fastest)
      stats[k].cells += 1
      if (byLang[k] === fastest) stats[k].wins += 1
    })
  })
  return Object.keys(stats)
    .map((lang) => ({
      lang,
      langDisplay: display[lang] || lang,
      geomean: Math.exp(stats[lang].logSum / stats[lang].cells),
      wins: stats[lang].wins,
      cells: stats[lang].cells,
    }))
    .sort((a, b) => a.geomean - b.geomean)
}

const LANG_COLORS: { [lang: string]: string } = {
  c: '#2563eb',
  cpp: '#7c3aed',
  go: '#0891b2',
  rust: '#ea580c',
  zig: '#ca8a04',
}

const PALETTE = ['#2563eb', '#7c3aed', '#0891b2', '#ea580c', '#ca8a04', '#16a34a', '#db2777', '#475569']

export function seriesColor(key: string, index: number): string {
  return LANG_COLORS[key] || PALETTE[index % PALETTE.length]
}

// Project-site base path (GitHub Pages serves this repo at
// /System-Programming-Language-Benchmarks/, not at the domain root).
// Internal links must go through this, or they point at the domain
// root and 404 on Pages. At the domain root the base is '/' and this
// is a no-op, so local dev and Vercel are unaffected.
export function withBase(router: any, path: string): string {
  const base =
    router && router.options && router.options.base ? router.options.base : '/'
  const clean = base.endsWith('/') ? base.slice(0, -1) : base
  return clean + (path.startsWith('/') ? path : '/' + path)
}

// One-line description per benchmark problem, shown on the problem's
// own page under its title. Kept here (not in the component) so the
// text is shared and easy to correct in one place. Unknown problems
// return '' so the page renders no empty line.
const PROBLEM_DESCRIPTIONS: { [problem: string]: string } = {
  binarytrees:
    'Allocate and free binary trees; stresses the memory allocator and garbage collection.',
  'coro-prime-sieve':
    'Generate primes with a coroutine-based Sieve of Eratosthenes.',
  edigits: 'Compute digits of e with arbitrary-precision arithmetic.',
  'fannkuch-redux':
    'Repeatedly flip prefixes of permutations; stresses array access and loops.',
  fasta: 'Generate weighted random DNA sequences in FASTA format.',
  helloworld: "Print 'Hello, World!' — the baseline sanity check.",
  'http-server': 'Serve simple HTTP responses; measures request handling.',
  'json-serde': 'Parse and re-emit JSON documents; measures parsing and string handling.',
  knucleotide: 'Count k-nucleotide frequencies in a DNA sequence.',
  lru: 'Exercise a least-recently-used cache with get/put churn.',
  mandelbrot: 'Compute and render the Mandelbrot set as a bitmap.',
  merkletrees: 'Build Merkle trees and verify hashes; stresses allocation and hashing.',
  nbody: 'Simulate planetary orbits with Newtonian gravity.',
  nsieve: 'Find primes with the Sieve of Eratosthenes over growing ranges.',
  pidigits: 'Compute digits of π with arbitrary-precision arithmetic.',
  'regex-redux': 'Apply regular-expression matching and substitution to DNA text.',
  secp256k1: 'Scalar multiplication on the secp256k1 elliptic curve.',
  'spectral-norm': 'Power iteration for the spectral norm of a matrix.',
}

export function problemDescription(problem: string): string {
  return PROBLEM_DESCRIPTIONS[problem] || ''
}
