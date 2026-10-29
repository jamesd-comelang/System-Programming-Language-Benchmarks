declare module '*.vue' {
  import V from 'vue'
  export default interface Vue extends V {}
}

type osType = 'linux' | 'osx' | 'windows'

type BenchResult = {
  cpuInfo: string
  lang: string
  os: osType
  compiler: string
  compilerVersion: string
  test: string
  input: string
  code: string
  timeMS: number
  timeStdDevMS: number
  memBytes: number
  cpuTimeMS: number
  cpuTimeUserMS: number
  cpuTimeKernelMS: number

  // Per-process IO totals from /proc/<pid>/io (Linux bench runs
  // only). Absent on results from before IO capture was added, and
  // zero where the platform does not report them.
  ioReadChars?: number
  ioWriteChars?: number
  ioReadBytes?: number
  ioWriteBytes?: number
  ioSyscalls?: number

  compilerOptions?: string

  par?: boolean
  timeout?: boolean

  // appveyorBuildId: string,
  githubRunId: string
  buildLog: {
    compilerVersion: string
    start: string
    finished: string
    durationMs: number
  }
  testLog: {
    runtimeVersion: string
    start: string
    finished: string
    durationMs: number
  }
}

type LangBenchResults = {
  lang: string
  langDisplay: string
  benchmarks: BenchResult[]
}

type LangPageMeta = {
  lang?: string
  other?: string
  problem?: string
}
