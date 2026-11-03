<template>
  <tabs-settings />
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.performance.title') }" />
    </template>
    <template #content>
      <div class="content" v-text="$t('settings.performance.info')" />
      <div v-if="snapshots.current" class="columns is-multiline">
        <div class="column is-half">
          <card-stats-server :snapshots="snapshots" />
        </div>
        <div class="column is-half">
          <card-stats-player :snapshots="snapshots" />
        </div>
        <div class="column is-full">
          <card-stats-speakers :snapshots="snapshots" />
        </div>
        <div class="column is-half">
          <card-stats-clock :snapshots="snapshots" />
        </div>
        <div class="column is-half">
          <card-stats-receiver :snapshots="snapshots" />
        </div>
      </div>
    </template>
  </content-with-heading>
</template>

<script setup>
import { onBeforeUnmount, onMounted, reactive } from 'vue'
import CardStatsClock from '@/components/CardStatsClock.vue'
import CardStatsPlayer from '@/components/CardStatsPlayer.vue'
import CardStatsReceiver from '@/components/CardStatsReceiver.vue'
import CardStatsServer from '@/components/CardStatsServer.vue'
import CardStatsSpeakers from '@/components/CardStatsSpeakers.vue'
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import stats from '@/api/stats'

const POLL_MS = 2000

// Two snapshots, so the cards can show rates
const snapshots = reactive({ current: null, previous: null })
let timer = null

const poll = async () => {
  try {
    const snapshot = await stats.get()
    snapshots.previous = snapshots.current
    snapshots.current = snapshot
  } catch {
    // The request interceptor already shows a notification
  }
}

onMounted(() => {
  poll()
  timer = setInterval(poll, POLL_MS)
})

onBeforeUnmount(() => clearInterval(timer))
</script>
