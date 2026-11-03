<template>
  <div class="box">
    <h3 class="title is-6" v-text="$t('settings.performance.server')" />
    <table class="table is-fullwidth is-narrow">
      <tbody>
        <tr>
          <td v-text="$t('settings.performance.cpu')" />
          <td class="has-text-right">
            <span :class="cpuClass" v-text="cpuText" />
          </td>
        </tr>
        <tr>
          <td v-text="$t('settings.performance.memory')" />
          <td
            class="has-text-right"
            v-text="formatKb(current.process?.rss_kb)"
          />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.load')" />
          <td class="has-text-right" v-text="loadText" />
        </tr>
        <tr v-if="current.host?.mem_total_kb">
          <td v-text="$t('settings.performance.available')" />
          <td class="has-text-right" v-text="memoryText" />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.uptime')" />
          <td
            class="has-text-right"
            v-text="formatDuration(current.process?.uptime_s)"
          />
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup>
import { cpuPercent, formatDuration, formatKb } from '@/lib/Stats'
import { computed } from 'vue'

const CPU_WARN = 50
const CPU_DANGER = 80

const props = defineProps({ snapshots: { required: true, type: Object } })

const current = computed(() => props.snapshots.current)
const cpu = computed(() =>
  cpuPercent(props.snapshots.current, props.snapshots.previous)
)

const cpuText = computed(() => {
  if (cpu.value === null) {
    return '…'
  }
  return `${cpu.value.toFixed(1)} %`
})

const cpuClass = computed(() => ({
  'has-text-danger': cpu.value > CPU_DANGER,
  'has-text-warning-dark': cpu.value > CPU_WARN && cpu.value <= CPU_DANGER
}))

const loadText = computed(() => {
  const { host } = current.value
  if (!host || typeof host.load_1m !== 'number') {
    return '–'
  }
  return `${host.load_1m.toFixed(2)} / ${host.load_5m.toFixed(2)} / ${host.load_15m.toFixed(2)} (${host.cpus} CPU)`
})

const memoryText = computed(
  () =>
    `${formatKb(current.value.host.mem_available_kb)} / ${formatKb(current.value.host.mem_total_kb)}`
)
</script>
