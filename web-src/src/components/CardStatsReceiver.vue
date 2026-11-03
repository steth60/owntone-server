<template>
  <div class="box">
    <h3 class="title is-6" v-text="$t('settings.performance.receiver')" />
    <p
      v-if="!receiver.enabled"
      class="is-size-7"
      v-text="$t('settings.performance.receiver-off')"
    />
    <table v-else class="table is-fullwidth is-narrow">
      <tbody>
        <tr>
          <td v-text="receiver.name" />
          <td class="has-text-right" v-text="senderText" />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.packets')" />
          <td class="has-text-right" v-text="receiver.packets_in" />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.undecryptable')" />
          <td class="has-text-right">
            <span
              :class="{ 'has-text-danger': receiver.packets_undecryptable > 0 }"
              v-text="receiver.packets_undecryptable"
            />
          </td>
        </tr>
        <tr>
          <td v-text="$t('settings.performance.dropped')" />
          <td class="has-text-right" v-text="receiver.packets_dropped" />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.queued')" />
          <td
            class="has-text-right"
            v-text="formatKb(receiver.queued_bytes / 1024)"
          />
        </tr>
        <template v-if="clock">
          <tr>
            <td v-text="$t('settings.performance.jitter')" />
            <td
              class="has-text-right"
              v-text="`${clock.jitter_us.toFixed(0)} µs`"
            />
          </tr>
          <tr>
            <td v-text="$t('settings.performance.drift')" />
            <td
              class="has-text-right"
              v-text="`${clock.drift_ppm.toFixed(2)} ppm`"
            />
          </tr>
          <tr>
            <td v-text="$t('settings.performance.steps')" />
            <td class="has-text-right" v-text="clock.steps" />
          </tr>
        </template>
      </tbody>
    </table>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { formatKb } from '@/lib/Stats'
import { useI18n } from 'vue-i18n'

const props = defineProps({ snapshots: { required: true, type: Object } })

const { t } = useI18n()

const receiver = computed(() => props.snapshots.current.receiver ?? {})
const clock = computed(() => receiver.value.sender_clock)

const senderText = computed(() => {
  if (receiver.value.streaming) {
    return receiver.value.sender
  }
  return t('settings.performance.idle')
})
</script>
