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
          <a :href="`/${i.lang}`" :class="getLinkClass()">{{
            i.langDisplay
          }}</a>
        </li>
      </ul>
    </aside>
    <article :class="contentClass">
      <div class="px-5 max-w-prose">
        <div class="text-lg tracking-widest leading-8">
          <p class="pt-5">
            This site provides side by side comparison of five system
            programming languages — C, C++, Rust, Go, and Zig — and their
            different compilers or runtimes
          </p>
          <p class="pt-5">
            It currently uses CI to generate benchmark results to guarantee all
            the numbers are generated from the same environment at nearly the
            same time. All benchmark tests are executed in a single CI job
          </p>
          <p class="pt-5">
            Once a change is merged into main branch, the CI job will
            re-generate and publish the static website
          </p>
          <p class="pt-5 font-bold">Main goals:</p>
          <ul class="list-disc list-outside italic text-base">
            <li class="pt-5">
              Compare performance differences between different system programming languages. Note
              that implementations might be using different optimizations, e.g.
              with or without multithreading, please do read the source code to
              check if it's a fair comparision or not.
            </li>
            <li class="pt-5">
              Compare performance differences between different compilers or
              runtimes of the same language with the same source code.
            </li>
            <li class="pt-5">
              Facilitate benchmarking on real server environments as nowadays
              more and more applications are deployed in hosted cloud VMs or
              docker/podman(via k8s). It's likely to get a very different result
              from what you get on your dev machine. You can simply use the tool
              to generate numbers on your PROD host environment to check if
              everything works as expected.
            </li>
            <li class="pt-5">
              A reference for CI setup / Dev environment setup / package
              management setup for system programming languages. Refer to
              <a
                href="https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks/blob/main/.github/workflows/bench.yml"
                class="underline text-blue-500"
                target="_blank"
                >Github action</a
              >
            </li>
            <li class="pt-5">
              It covers only system programming languages: C, C++, Rust, Go,
              and Zig. Other languages from the upstream project are not
              included here. Benchmark problems are borrowed from
              <a
                href="https://benchmarksgame-team.pages.debian.net/benchmarksgame/index.html"
                class="underline text-blue-500"
                target="_blank"
                >CLBG</a
              >.
            </li>
          </ul>
        </div>
        <div class="mt-8">
          <h2 class="text-2xl font-bold">Fastest overall</h2>
          <p class="text-sm italic leading-6 mt-2">
            Average slowness vs the fastest language, single-thread results
            only. 1.00× means fastest in every counted cell; timeouts are
            excluded, and a cell only counts when at least two languages have
            a result. Languages cover different problems, so compare the
            "cells" column too — a score from few cells is less comparable
            than one from many.
          </p>
          <table class="table-auto w-full text-base leading-loose mt-3">
            <tr class="border-b-2 border-dotted">
              <th class="text-left">Rank</th>
              <th class="text-left">Language</th>
              <th class="text-right">Avg × vs fastest</th>
              <th class="text-right">Wins</th>
              <th class="text-right">Cells</th>
            </tr>
            <tbody>
              <tr
                v-for="(row, idx) in leaderboardRows"
                :key="row.lang"
                class="border-b-2 border-dotted"
              >
                <td>{{ idx + 1 }}</td>
                <td>
                  <a
                    :href="`/${row.lang}`"
                    class="underline text-blue-500"
                    >{{ row.langDisplay }}</a
                  >
                </td>
                <td class="text-right font-bold">
                  {{ row.geomean.toFixed(2) }}×
                </td>
                <td class="text-right">{{ row.wins }}</td>
                <td class="text-right">{{ row.cells }}</td>
              </tr>
            </tbody>
          </table>
          <p class="text-sm italic leading-6 mt-2">
            This is a summary, not a verdict: click a language for the
            per-problem tables, where the fastest implementation for each
            input is shown with its × vs fastest.
          </p>
        </div>
        <div class="italic text-base leading-8">
          <p class="pt-5">
            Current benchmark data was generated on
            <span class="text-pink-800">{{ benchmarkDate }}</span
            >, full log can be found
            <a
              :href="buildLogUrl"
              target="_blank"
              class="underline bold text-blue-500"
              >HERE</a
            >
          </p>
          <p class="pt-5">
            It's inspired by
            <a
              class="underline text-blue-500"
              href="https://benchmarksgame-team.pages.debian.net/benchmarksgame/index.html"
              target="_blank"
              >Benchmarks Game</a
            >, some of the benchmark problems and implementation are borrowed
            from it.
          </p>
          <p class="pt-5">
            <a
              class="underline bold text-blue-500"
              href="https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks"
              target="_blank"
              >CONTRIBUTIONS</a
            >
            are WELCOME!
          </p>
        </div>
      </div>
    </article>
    <aside :class="sideBarClass">
      <div>
        <h2 class="text-xl">Problems</h2>
        <ul class="text-base">
          <li
            v-for="(i, idx) in problems"
            :key="idx"
            class="text-light-onSurfacePrimary"
          >
            <a
              :href="`/problem/${i}`"
              class="p-1 pl-3 flex rounded transition-colors duration-300 ease-linear justify-between underline text-blue-500 hover:text-green-400"
            >
              {{ i }}</a
            >
          </li>
        </ul>
      </div>
    </aside>
  </div>
</template>

<script lang="ts">
import { Component, Vue } from 'nuxt-property-decorator'
import _ from 'lodash'
import { MetaInfo } from 'vue-meta'
import MenuButton from './../components/MenuButton.vue'
import { mergeLangBenchResults } from '~/contentUtils'
import { leaderboard } from '~/resultSummary'
import { importGoogleTagIfNeeded } from '~/gtmUtils'

function requireAll(requireContext: any) {
  const r = requireContext.keys().map(requireContext)
  return mergeLangBenchResults(r)
}
const langs = requireAll((require as any).context('../content', true, /.json$/))

Component.registerHooks(['head'])

@Component({
  components: {
    MenuButton,
  },
})
export default class IndexPage extends Vue {
  isMenuOn = false
  langs: LangBenchResults[] = langs
  problems: string[] = []

  toggleMenu() {
    this.isMenuOn = !this.isMenuOn
  }

  get leaderboardRows() {
    return leaderboard(this.langs)
  }

  get sideBarClass() {
    return this.isMenuOn ? 'block w-1/6 half-width' : 'block w-1/6 md-hide'
  }

  get contentClass() {
    return this.isMenuOn
      ? 'block w-4/6 md-hide'
      : 'block w-4/6 full-width mx-auto'
  }

  created() {
    this.problems = _.chain(this.langs)
      .map((i) => i.benchmarks)
      .flatten()
      .map((i) => i.test)
      .uniq()
      .sort()
      .value()
  }

  head() {
    const langsStrs = _.chain(this.langs)
      .map((i) => i.langDisplay)
      .uniq()
      .value()

    const metaInfo: MetaInfo = {
      title:
        'Benchmarks for system programming languages and compilers, Which system programming language or compiler is faster',
      meta: [
        {
          hid: 'description',
          name: 'description',
          content: `system programming languages,benchmark,benchmarks,performance,${langsStrs.join(
            ','
          )},${langsStrs.join(' lang,')} lang,${this.problems.join(',')}`,
        },
      ],
    }
    importGoogleTagIfNeeded(metaInfo)
    return metaInfo
  }

  getLinkClass() {
    const baseClass =
      'p-1 pl-3 flex rounded transition-colors duration-300 ease-linear justify-between'
    // const activeClass = ' text-gray-700 cursor-default'
    const inactiveClass = ' underline text-blue-500 hover:text-green-400'
    return baseClass + inactiveClass
  }

  get buildLogUrl() {
    // const buildId = this.activeBenchmarks[0].appveyorBuildId
    const runId = this.langs[0].benchmarks[0].githubRunId
    return `https://github.com/jamesd-comelang/System-Programming-Language-Benchmarks/actions/runs/${runId}`
  }

  get benchmarkDate() {
    const ts = this.langs[0].benchmarks[0].testLog.finished as string
    return new Date(ts).toDateString()
  }
}
</script>
