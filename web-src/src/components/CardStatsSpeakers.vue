<template>
  <div class="box">
    <h3 class="title is-6" v-text="$t('settings.performance.speakers')" />
    <div class="table-container">
      <table class="table is-fullwidth is-narrow is-hoverable">
        <thead>
          <tr>
            <th v-text="$t('settings.performance.speaker')" />
            <th v-text="$t('settings.performance.transport')" />
            <th v-text="$t('settings.performance.eq')" />
            <th
              class="has-text-right"
              v-text="$t('settings.performance.rate')"
            />
            <th
              class="has-text-right"
              v-text="$t('settings.performance.retransmits')"
            />
            <th
              class="has-text-right"
              v-text="$t('settings.performance.missed')"
            />
            <th
              class="has-text-right"
              v-text="$t('settings.performance.failures')"
            />
            <th v-text="$t('settings.performance.last-event')" />
          </tr>
        </thead>
        <tbody>
          <tr v-if="!rows.length">
            <td colspan="8" v-text="$t('settings.performance.none')" />
          </tr>
          <tr v-for="row in rows" :key="row.id">
            <td>
              <span class="dot" :class="row.selected ? 'is-on' : 'is-off'" />
              <span v-text="row.name" />
            </td>
            <td v-text="row.transport" />
            <td>
              <span
                v-if="row.eq"
                class="tag is-info is-light"
                v-text="$t('settings.performance.eq')"
              />
              <span
                v-if="row.clipping"
                class="tag is-warning is-light ml-1"
                v-text="$t('settings.eq.clipping')"
              />
            </td>
            <td class="has-text-right" v-text="row.rateText" />
            <td class="has-text-right">
              <span
                :class="{ 'has-text-warning-dark': row.resending }"
                v-text="row.retransmits"
              />
            </td>
            <td class="has-text-right">
              <span
                :class="{ 'has-text-danger': row.lost }"
                v-text="row.missed"
              />
            </td>
            <td class="has-text-right">
              <span
                :class="{ 'has-text-danger': row.failuresIncreased }"
                v-text="row.failures"
              />
            </td>
            <td class="is-size-7" v-text="row.event" />
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { speakerRows } from '@/lib/Stats'

const props = defineProps({ snapshots: { required: true, type: Object } })

const rows = computed(() =>
  speakerRows(props.snapshots.current, props.snapshots.previous)
)
</script>

<style scoped>
.dot {
  display: inline-block;
  width: 0.5rem;
  height: 0.5rem;
  border-radius: 50%;
  margin-right: 0.4rem;
  vertical-align: middle;
}
.dot.is-on {
  background: var(--bulma-success);
}
.dot.is-off {
  background: var(--bulma-border);
}
</style>
