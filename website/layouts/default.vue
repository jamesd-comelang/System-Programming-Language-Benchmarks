<template>
  <div
    class="text-gray-700 text-base subpixel-antialiased text-lg tracking-wide .leading-relaxed py-5 md:container md:mx-auto min-h-screen shadow-lg bg-gray-100 p-5"
  >
    <div class="bg-pink-800 text-white p-5 mb-5 text-3xl rounded">
      <a :href="u('/')"
        ><small class="text-lg">System Programming Language and compiler</small> <br />
        Benchmarks</a
      >
    </div>
    <Nuxt />
  </div>
</template>

<script lang="ts">
import { Component, Vue } from 'nuxt-property-decorator'
import { withBase } from '~/resultSummary'
@Component({
  components: {},
})
export default class DefaultLayout extends Vue {
  u(path: string) {
    return withBase(this.$router, path)
  }
}
</script>
