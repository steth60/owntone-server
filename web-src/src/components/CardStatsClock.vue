<template>
  <div class="box">
    <h3 class="title is-6" v-text="$t('settings.performance.clock')" />
    <p class="is-size-7 mb-2" v-text="$t('settings.performance.clock-info')" />
    <table class="table is-fullwidth is-narrow">
      <thead>
        <tr>
          <th v-text="$t('settings.performance.peer')" />
          <th
            class="has-text-right"
            v-text="$t('settings.performance.last-heard')"
          />
        </tr>
      </thead>
      <tbody>
        <tr v-if="!peers.length">
          <td colspan="2" v-text="$t('settings.performance.none')" />
        </tr>
        <tr v-for="peer in peers" :key="peer.address">
          <td v-text="peer.address" />
          <td class="has-text-right">
            <span
              v-text="
                $t('settings.performance.ago', { n: peer.last_seen_s_ago })
              "
            />
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup>
import { computed } from 'vue'

const props = defineProps({ snapshots: { required: true, type: Object } })

const peers = computed(() => props.snapshots.current.ptp?.peers ?? [])
</script>
