<template>
  <div class="flex flex-wrap relative">
    <menu-button @toggle="toggleMenu"></menu-button>
    <aside :class="sideBarClass">
      <h2 title="Benchmarks for system programming languages" class="text-xl">
        Languages
      </h2>
      <ul class="text-base">
        <li
          v-for="(i, idx) in langs"
          :key="idx"
          class="text-light-onSurfacePrimary"
        >
          <a
            v-if="!isLinkActive(i.lang, '')"
            :href="u('/' + i.lang)"
            :class="getLinkClass(i.lang, '')"
            >{{ i.langDisplay }}</a
          >
          <span
            v-if="isLinkActive(i.lang, '')"
            :class="getLinkClass(i.lang, '')"
            >{{ i.langDisplay }}</span
          >
        </li>
      </ul>
      <!-- Page info, kept short so it fits this narrow column. -->
      <div class="mt-8 text-xs">
        <p class="italic leading-relaxed">
          Data: <span class="text-pink-800">{{ benchmarkDate }}</span
          >, full log
          <a
            :href="buildLogUrl"
            target="_blank"
            class="underline bold text-blue-500"
            >HERE</a
          >
        </p>
        <p class="mt-2 italic">
          <a
            class="underline bold text-blue-500"
            href="https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks"
            target="_blank"
            >CONTRIBUTIONS</a
          >
          welcome!
        </p>
        <div v-if="cpuInfo" class="mt-4">
          <label class="font-bold">CPU:</label>
          <span class="italic">{{ shortCpuInfo }}</span>
        </div>
        <div class="mt-4">
          <p class="italic">-m: multi-threaded</p>
          <p class="italic">-i: direct intrinsics</p>
          <p class="italic">
            -ffi: non-stdlib
            <a
              href="http://en.wikipedia.org/wiki/Foreign_function_interface"
              class="underline text-blue-500"
              >FFI</a
            >
          </p>
          <p class="italic">
            time can be under user+sys time: GC/JIT may use extra cores.
          </p>
        </div>
        <div class="mt-4">
          <div class="form-check form-check-inline">
            <input
              v-model="show_st"
              class="form-check-input inline-block"
              type="checkbox"
              style="vertical-align: bottom"
            />
            <label class="form-check-label inline-block text-gray-500"
              >without parallelization</label
            >
          </div>
          <div class="mt-2 form-check form-check-inline">
            <input
              v-model="show_mt"
              class="form-check-input inline-block"
              type="checkbox"
              style="vertical-align: bottom"
            />
            <label class="form-check-label inline-block text-gray-500"
              >with parallelization</label
            >
          </div>
          <div class="mt-2 form-check form-check-inline">
            <input
              v-model="showRaw"
              class="form-check-input inline-block"
              type="checkbox"
              style="vertical-align: bottom"
            />
            <label class="form-check-label inline-block text-gray-500"
              >raw result tables</label
            >
          </div>
          <div class="mt-2 form-check form-check-inline">
            <input
              v-model="chartLog"
              class="form-check-input inline-block"
              type="checkbox"
              style="vertical-align: bottom"
            />
            <label class="form-check-label inline-block text-gray-500"
              >log scale charts</label
            >
          </div>
        </div>
      </div>
    </aside>
    <div :class="contentClass">
      <h1 v-if="lang && !other" class="text-3xl">
        All {{ lang.langDisplay }} benchmarks
      </h1>
      <h1 v-if="other" class="text-3xl">
        {{ lang.langDisplay }} VS {{ other.langDisplay }} benchmarks
      </h1>
      <h1 v-if="problem" class="text-3xl">
        All {{ problem }} problem benchmarks
      </h1>
      <p
        v-if="problem && problemDescriptionText"
        class="text-sm italic mt-2"
      >
        {{ problemDescriptionText }}
      </p>
      <div v-for="test in testOptions" :key="'matrix-' + test">
        <h2 class="text-2xl my-5 mb-2 underline text-blue-500">
          <a :id="'matrix-' + test" :href="u('/problem/' + test)"> {{ test }} </a>
        </h2>
        <p class="text-sm italic">{{ matrixSummary(test) }}</p>
        <p class="text-sm italic">
          Each cell is the fastest time for that row in that input. The
          number in brackets is × vs the fastest row in the same input
          column (1.00× is fastest); the bar length is proportional to
          time, so a longer bar is slower. — means no result, timeout
          means the run timed out.
        </p>
        <table class="table-auto w-full text-sm leading-loose mt-2">
          <tr class="border-b-2 border-dotted">
            <th class="text-left">Series</th>
            <th
              v-for="input in matrixInputs(test)"
              :key="input"
              class="text-right"
            >
              Input: {{ input }}
            </th>
          </tr>
          <tbody>
            <tr
              v-for="(row, ridx) in matrixRows(test)"
              :key="row.key"
              class="border-b-2 border-dotted"
            >
              <td class="text-left font-bold">
                <span :style="{ color: colorFor(row.key, ridx) }">■</span>
                {{ row.label }}
              </td>
              <td
                v-for="input in matrixInputs(test)"
                :key="input"
                class="text-right"
              >
                <span v-if="row.cells[input].missing">—</span>
                <span v-else-if="row.cells[input].timeout">timeout</span>
                <span v-else>
                  <span :class="row.cells[input].ratio === 1 ? 'font-bold' : ''"
                    >{{ msToText(row.cells[input].timeMS) }} ({{
                      row.cells[input].ratio.toFixed(2)
                    }}×)</span
                  >
                  <div class="bg-gray-200" style="height: 6px; margin-top: 2px">
                    <div
                      :style="{
                        width:
                          matrixBarWidth(
                            test,
                            input,
                            row.cells[input].timeMS
                          ) + '%',
                        background: colorFor(row.key, ridx),
                        height: '6px',
                      }"
                    ></div>
                  </div>
                </span>
              </td>
            </tr>
          </tbody>
        </table>
        <div class="mt-4">
          <h3 class="text-base font-bold">
            Chart — largest input: {{ largestMatrixInput(test) }}
          </h3>
          <div
            v-for="(row, ridx) in chartRows(test)"
            :key="'chart-' + row.key"
            class="flex items-center mt-1 text-sm"
          >
            <span class="w-32 text-right pr-2 truncate" :title="row.label">{{
              row.label
            }}</span>
            <div class="flex-1 bg-gray-200" style="height: 14px">
              <div
                v-if="row.timeMS !== null"
                :style="{
                  width: chartBarWidth(test, row) + '%',
                  background: colorFor(row.key, ridx),
                  height: '14px',
                }"
              ></div>
            </div>
            <span class="w-40 pl-2">{{
              row.timeMS === null
                ? row.timeout
                  ? 'timeout'
                  : '—'
                : msToText(row.timeMS) + ' (' + row.ratio.toFixed(2) + '×)'
            }}</span>
          </div>
        </div>
      </div>
      <div v-show="showRaw" v-for="test in testOptions" :key="test">
        <h2 class="text-2xl my-5 mb-2 underline text-blue-500">
          <a :id="test" :href="u('/problem/' + test)"> {{ test }} </a>
        </h2>
        <div v-for="input in getInputs(test)" :key="input" class="mt-5">
          <h3 class="text-base font-bold text-red-800">Input: {{ input }}</h3>
          <table class="table-auto w-full text-xs leading-loose">
            <tr class="border-b-2 border-dotted py-1">
              <th
                v-show="other || problem"
                :class="['text-left', 'pl-4', mdHide]"
              >
                lang
              </th>
              <th class="text-right">code</th>
              <!-- <th class="text-right">N</th> -->
              <th class="text-right" title="total time">time</th>
              <th
                :class="['text-right', mdHide]"
                title="total time standard deviation"
              >
                stddev
              </th>
              <th class="text-right">
                <span class="md-hide">peak-mem</span>
                <span class="md-show">mem</span>
              </th>
              <th :class="['text-right', mdHide]" title="cpu-time-user">
                time(user)
              </th>
              <th :class="['text-right', mdHide]" title="cpu-time-kernel">
                time(sys)
              </th>
              <th class="text-left pl-5">
                <span class="md-show">compiler</span>
                <span class="md-hide">compiler/runtime</span>
              </th>
            </tr>
            <tbody class="font-light">
              <tr
                v-for="(i, idx) in filterBenches(test, input)"
                :key="idx"
                :class="
                  'border-b-2 border-dotted py-1 ' +
                  (idx % 2 == 0 ? 'bg-gray-200' : '')
                "
              >
                <td
                  v-show="other || problem"
                  :class="['text-left', 'pl-4', mdHide]"
                >
                  <a :href="u('/' + i.lang)" class="underline text-blue-500">{{
                    i.lang
                  }}</a>
                </td>
                <td class="text-right">
                  <a
                    :href="`https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks/blob/main/bench/algorithm/${test}/${i.code}`"
                    target="_blank"
                    class="underline text-blue-500"
                    >{{ getNormalizedCode(i) }}</a
                  >
                </td>
                <!-- <td class="text-right">{{ i.input }}</td> -->
                <td class="text-right">
                  {{ msToText(i.timeMS) }}
                </td>
                <td :class="['text-right', mdHide]">
                  {{ msToFixed(i.timeStdDevMS) }}ms
                </td>
                <td class="text-right">
                  {{ (i.memBytes / (1024 * 1024)).toFixed(1) }}MB
                </td>
                <td :class="['text-right', mdHide]">
                  {{ i.cpuTimeUserMS.toFixed(0) }}ms
                </td>
                <td :class="['text-right', mdHide]">
                  {{ i.cpuTimeKernelMS.toFixed(0) }}ms
                </td>
                <td class="text-left pl-5" :title="getFullCompilerVersion(i)">
                  {{ i.compiler }} {{ i.compilerVersion }}
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>
    <aside :class="sideBarClass">
      <div class="pb-5">
        <h2 class="text-xl">Problems</h2>
        <ul class="text-base">
          <li
            v-for="(i, idx) in allProblems"
            :key="idx"
            class="text-light-onSurfacePrimary"
          >
            <a
              v-if="i != problem"
              :href="u('/problem/' + i)"
              :class="getLinkClass(i, i)"
            >
              {{ i }}</a
            >
            <span v-if="i == problem" :class="getLinkClass(i, i)">{{ i }}</span>
          </li>
        </ul>
      </div>
      <div v-if="!problem">
        <h2 class="text-xl">Compare</h2>
        <ul class="text-base">
          <li
            v-for="(i, idx) in otherLangs"
            :key="idx"
            class="text-light-onSurfacePrimary"
          >
            <a
              v-if="!isLinkActive(lang.lang, i.lang)"
              :href="u('/' + lang.lang + '-vs-' + i.lang)"
              :class="getLinkClass(lang.lang, i.lang)"
            >
              {{ lang.langDisplay }} VS {{ i.langDisplay }}</a
            >
            <span
              v-if="isLinkActive(lang.lang, i.lang)"
              :class="getLinkClass(lang.lang, i.lang)"
              >{{ lang.langDisplay }} VS {{ i.langDisplay }}</span
            >
          </li>
        </ul>
      </div>
    </aside>
  </div>
</template>
<script lang="ts">
import { Component, Watch, Vue } from 'nuxt-property-decorator'
import _ from 'lodash'
import { MetaInfo } from 'vue-meta'
import MenuButton from './MenuButton.vue'
import { getFullCompilerVersion, mergeLangBenchResults } from '~/contentUtils'
import {
  barWidth,
  isVisible,
  largestInput,
  matrixForProblem,
  problemDescription,
  seriesColor as seriesColorFn,
  withBase,
  sortInputs,
} from '~/resultSummary'
import { importGoogleTagIfNeeded } from '~/gtmUtils'

function requireAll(requireContext: any) {
  const r = requireContext.keys().map(requireContext)
  return mergeLangBenchResults(r)
}
const langs = requireAll((require as any).context('../content', true, /.json$/))
const problems = _.chain(langs)
  .flatMap((i) => i.benchmarks)
  .map((i) => i.test)
  .uniq()
  .sort()
  .value()

Component.registerHooks(['head'])

@Component({
  components: {
    MenuButton,
  },
})
export default class LangMetaPage extends Vue {
  isMenuOn = false
  show_st = true
  show_mt = true
  showRaw = false
  chartLog = false
  meta?: LangPageMeta
  problem?: string
  allProblems?: string[] = problems
  lang?: LangBenchResults
  other?: LangBenchResults
  langs: LangBenchResults[] = langs

  activeBenchmarks: BenchResult[] = []

  testOptions: string[] = []

  osOptions: string[] = []
  compilerOptions: string[] = []
  compilerVersionOptions: string[] = []
  compilerOptionOptions: string[] = []

  osSelected = ''
  compilerSelected = ''
  compilerVersionSelected = ''
  compilerOptionSelected = ''

  toggleMenu() {
    this.isMenuOn = !this.isMenuOn
  }

  msToText(ms: number): string {
    return ms <= 0 ? 'timeout' : `${this.msToFixed(ms)}ms`
  }

  msToFixed(ms: number): string {
    return ms < 10 ? ms.toFixed(1) : ms.toFixed(0)
  }

  get cpuInfo() {
    return this.activeBenchmarks[0].cpuInfo
  }

  // Bench CPU strings look like "[x86_64][4 cores] AMD EPYC 9V45
  // 96-Core Processor (Model 2)". The sidebar is narrow, so show
  // only the model and the core count.
  get shortCpuInfo() {
    const raw = this.cpuInfo || ''
    const cores = /\[(\d+)\s*cores?\]/.exec(raw)
    const model = raw
      .replace(/\[[^\]]*\]/g, '')
      .replace(/\s*Processor.*$/, '')
      .trim()
    if (!model) return raw
    return cores ? `${model} · ${cores[1]} cores` : model
  }

  get problemDescriptionText() {
    return this.problem ? problemDescription(this.problem) : ''
  }

  get sideBarClass() {
    return this.isMenuOn ? 'block w-1/6 half-width' : 'block w-1/6 md-hide'
  }

  get contentClass() {
    return this.isMenuOn
      ? 'block w-4/6 pr-4 md-hide'
      : 'block w-4/6 full-width mx-auto'
  }

  get mdHide() {
    return 'md-hide'
  }

  get otherLangs() {
    return _.chain(this.langs)
      .filter((i) => i.lang !== this.lang?.lang)
      .value()
  }

  get buildLogUrl() {
    // const buildId = this.activeBenchmarks[0].appveyorBuildId
    const runId = this.activeBenchmarks[0].githubRunId
    return `https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks/actions/runs/${runId}`
  }

  get benchmarkDate() {
    const ts = this.activeBenchmarks[0].testLog.finished as string
    return new Date(ts).toDateString()
  }

  getFullCompilerVersion(i: BenchResult) {
    return getFullCompilerVersion(i)
  }

  isLinkActive(lang: string, otherLang: string) {
    if (this.problem) {
      // HACK: use lang or otherLang to represent current problem
      return this.problem === lang
    } else if (otherLang && this.other) {
      return this.other?.lang === otherLang
    } else if (!otherLang && !this.other) {
      return this.lang?.lang === lang
    }

    return false
  }

  getLinkClass(lang: string, otherLang: string) {
    const baseClass =
      'p-1 pl-3 flex rounded transition-colors duration-300 ease-linear justify-between'
    const activeClass = ' text-gray-700 cursor-default'
    const inactiveClass = ' underline text-blue-500 hover:text-green-400'
    return (
      baseClass +
      (this.isLinkActive(lang, otherLang) ? activeClass : inactiveClass)
    )
  }

  getInputs(test: string) {
    return _.chain(this.activeBenchmarks)
      .filter((i) => i.test === test && i.os === this.osSelected)
      .map((i) => i.input)
      .uniq()
      .orderBy((i) => parseInt(i) ?? i, 'desc')
      .value()
  }

  filterBenches(test: string, input: string) {
    let exp = _.chain(this.activeBenchmarks).filter(
      (i) => i.test === test && i.os === this.osSelected && i.input === input
    )

    if (this.other?.benchmarks && this.other?.benchmarks?.length > 0) {
      exp = exp.unionWith(
        _.chain(this.other?.benchmarks)
          .filter(
            (i) =>
              i.test === test && i.os === this.osSelected && i.input === input
          )
          .value()
      )
    }

    if (!this.problem) {
      // Lang page sort
      exp = exp.orderBy(
        [
          'input',
          'timeout',
          'timeMS',
          'os',
          'lang',
          'compiler',
          'compilerVersion',
        ],
        ['asc', 'asc', 'asc', 'asc', 'asc', 'asc', 'asc']
      )
    }

    exp = exp.filter(
      (i) => (!i.par && this.show_st) || (!!i.par && this.show_mt)
    )

    return exp.value()
  }

  u(path: string) {
    return withBase(this.$router, path)
  }

  matrixBenches(test: string) {
    let benches = this.activeBenchmarks.filter(
      (i) => i.test === test && i.os === this.osSelected
    )
    if (this.other?.benchmarks && this.other?.benchmarks?.length > 0) {
      benches = benches.concat(
        this.other.benchmarks.filter(
          (i) => i.test === test && i.os === this.osSelected
        )
      )
    }
    return benches.filter((i) => isVisible(i, this.show_st, this.show_mt))
  }

  matrixSeriesKey(i: BenchResult) {
    if (this.problem || this.other) return i.lang
    return `${i.compiler} ${i.code}`
  }

  matrixSeriesLabel(key: string) {
    if (this.problem || this.other) {
      const found = this.langs.filter((l) => l.lang === key)[0]
      return found ? found.langDisplay : key
    }
    return key
  }

  matrixInputs(test: string) {
    return sortInputs(
      _.chain(this.matrixBenches(test))
        .map((i) => i.input)
        .uniq()
        .value()
    )
  }

  matrixRows(test: string) {
    return matrixForProblem(
      this.matrixBenches(test),
      this.matrixInputs(test),
      (i) => this.matrixSeriesKey(i),
      (key) => this.matrixSeriesLabel(key)
    )
  }

  matrixSummary(test: string) {
    const rows = this.matrixRows(test)
    const inputs = this.matrixInputs(test)
    if (rows.length === 0 || inputs.length === 0) return 'No results.'
    let best = rows[0]
    rows.forEach((r) => {
      if (r.wins > best.wins) best = r
    })
    if (best.wins === 0) return `No fastest row across ${inputs.length} input(s).`
    return `Fastest in ${best.wins} of ${inputs.length} input(s): ${best.label}.`
  }

  largestMatrixInput(test: string) {
    return largestInput(this.matrixInputs(test))
  }

  chartRows(test: string) {
    const input = this.largestMatrixInput(test)
    const rows = this.matrixRows(test)
    return rows
      .map((r) => {
        const cell = r.cells[input]
        return {
          key: r.key,
          label: r.label,
          timeMS: cell ? cell.timeMS : null,
          ratio: cell ? cell.ratio : null,
          timeout: cell ? cell.timeout : false,
          missing: !cell || cell.missing,
        }
      })
      .sort((a, b) => {
        if (a.timeMS === null && b.timeMS === null) return 0
        if (a.timeMS === null) return 1
        if (b.timeMS === null) return -1
        return a.timeMS - b.timeMS
      })
  }

  colorFor(key: string, index: number) {
    return seriesColorFn(key, index)
  }

  matrixBarWidth(test: string, input: string, timeMS: number) {
    const rows = this.matrixRows(test)
    const times = rows
      .map((r) => r.cells[input])
      .filter((c) => c && c.timeMS !== null)
      .map((c) => c.timeMS as number)
    if (times.length === 0) return 0
    const fastest = Math.min.apply(null, times)
    const slowest = Math.max.apply(null, times)
    return barWidth(timeMS, fastest, slowest, false)
  }

  chartBarWidth(test: string, row: any) {
    const timed = this.chartRows(test).filter((r) => r.timeMS !== null)
    if (timed.length === 0 || row.timeMS === null) return 0
    const fastest = Math.min.apply(
      null,
      timed.map((r) => r.timeMS as number)
    )
    const slowest = Math.max.apply(
      null,
      timed.map((r) => r.timeMS as number)
    )
    return barWidth(row.timeMS, fastest, slowest, this.chartLog)
  }

  getNormalizedCode(i: BenchResult) {
    return !i.code.includes('-') && i.par ? i.code.replace('.', '-m.') : i.code
  }

  @Watch('compilerSelected')
  onPropertyChanged(value: string, oldValue: string) {
    if (value !== oldValue) {
      this.compilerVersionOptions = _.chain(this.activeBenchmarks)
        .filter((i) => i.compiler === this.compilerSelected)
        .map((i) => i.compilerVersion)
        .uniq()
        .value()
      this.compilerVersionSelected = this.compilerVersionOptions[0]
    }
  }

  head() {
    const suffix =
      'benchmarks, Which system programming language or compiler is faster'
    let title = ''
    if (this.problem) {
      title = `${this.problem} - ${suffix}`
    } else {
      title = `${this.lang?.langDisplay}${
        this.other ? ' VS ' + this.other?.langDisplay : ''
      } ${suffix}`
    }

    let langsStrs: string[] = []
    if (this.problem) {
      langsStrs = _.chain(this.langs)
        .map((i) => i.langDisplay)
        .uniq()
        .unionWith(
          _.chain(this.langs)
            .map((i) => `${i.langDisplay} lang`)
            .uniq()
            .value()
        )
        .value()
      langsStrs.push(this.problem)
    } else if (this.other) {
      langsStrs = _.chain(this.lang?.benchmarks)
        .map((i) => i.test)
        .uniq()
        .value()
      langsStrs.push(this.lang!.langDisplay)
      langsStrs.push(`${this.lang!.langDisplay} lang`)
      langsStrs.push(this.other.langDisplay)
      langsStrs.push(`${this.other.langDisplay} lang`)
    } else {
      langsStrs = _.chain(this.lang?.benchmarks)
        .map((i) => i.test)
        .uniq()
        .value()
      langsStrs.push(this.lang!.langDisplay)
      langsStrs.push(`${this.lang!.langDisplay} lang`)
    }

    const metaInfo: MetaInfo = {
      title,
      meta: [
        {
          hid: 'description',
          name: 'description',
          content: `benchmarks,benchmark,performance,${langsStrs.join(',')}`,
        },
      ],
    }
    importGoogleTagIfNeeded(metaInfo)
    return metaInfo
  }

  created() {
    this.meta = this.$route.meta
    this.problem = this.meta?.problem
    if (this.meta?.lang) {
      this.lang = _.chain(langs).find({ lang: this.meta.lang }).value()
    }
    if (this.meta?.other) {
      this.other = _.chain(langs).find({ lang: this.meta.other }).value()
    }

    this.activeBenchmarks =
      this.lang?.benchmarks ??
      _.chain(this.langs)
        .flatMap((i) => i.benchmarks)
        .filter((i) => i.test === this.problem)
        .orderBy(['input', 'timeout', 'timeMS'], ['asc', 'asc', 'asc'])
        .value()

    this.testOptions = _.chain(this.activeBenchmarks)
      .map((i) => i.test)
      .uniq()
      .value()

    this.osOptions = _.chain(this.activeBenchmarks)
      .map((i) => i.os)
      .uniq()
      .value()
    this.osSelected = this.osOptions[0]

    this.compilerOptions = _.chain(this.activeBenchmarks)
      .map((i) => i.compiler)
      .uniq()
      .value()
    this.compilerSelected = this.compilerOptions[0]
  }
}
</script>
