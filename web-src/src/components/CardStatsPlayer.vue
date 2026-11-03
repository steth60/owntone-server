<template>
  <div class="box">
    <h3 class="title is-6" v-text="$t('settings.performance.player')" />
    <table class="table is-fullwidth is-narrow">
      <tbody>
        <tr>
          <td v-text="$t('settings.performance.state')" />
          <td class="has-text-right" v-text="player.state" />
        </tr>
        <tr>
          <td v-text="$t('settings.performance.volume')" />
          <td class="has-text-right" v-text="player.volume" />
        </tr>
        <tr v-for="counter in COUNTERS" :key="counter">
          <td
            v-text="$t(`settings.performance.${counter.replace('_', '-')}`)"
          />
          <td class="has-text-right">
            <span
              :class="{
                'has-text-danger': counterIncreased(
                  snapshots,
                  'player',
                  counter
                )
              }"
              v-text="player[counter] ?? 0"
            />
          </td>
        </tr>
      </tbody>
    </table>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { counterIncreased } from '@/lib/Stats'

const COUNTERS = ['underruns', 'flushes', 'starts', 'read_errors']

const props = defineProps({ snapshots: { required: true, type: Object } })

const player = computed(() => props.snapshots.current.player ?? {})
</script>
