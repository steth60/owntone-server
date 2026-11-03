<template>
  <tabs-settings />
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.eq.title') }" />
    </template>
    <template #content>
      <div class="content" v-text="$t('settings.eq.info')" />
      <div v-if="!speakers.length" v-text="$t('settings.eq.no-speakers')" />
      <control-speaker-eq
        v-for="output in speakers"
        :key="output.id"
        :output="output"
      />
    </template>
  </content-with-heading>
</template>

<script setup>
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import ControlSpeakerEq from '@/components/ControlSpeakerEq.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import { computed } from 'vue'
import { useOutputsStore } from '@/stores/outputs'

const outputsStore = useOutputsStore()

// EQ is applied in the AirPlay output
const speakers = computed(() =>
  outputsStore.outputs.filter((output) => output.type?.startsWith('AirPlay'))
)
</script>
