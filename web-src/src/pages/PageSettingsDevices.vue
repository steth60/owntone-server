<template>
  <tabs-settings />
  <content-with-heading>
    <template #heading>
      <pane-title :content="{ title: $t('settings.devices.pairing') }" />
    </template>
    <template #content>
      <form v-if="remotesStore.active" @submit.prevent="pairRemote">
        <label class="label has-text-weight-normal content">
          <span v-text="$t('settings.devices.pairing-request')" />
          <b v-text="remotesStore.remote" />
        </label>
        <control-pin-field
          :placeholder="$t('dialog.remote-pairing.pairing-code')"
          @input="onRemotePinChange"
        >
          <div class="control">
            <button
              class="button"
              type="submit"
              :disabled="remotePairingDisabled"
              v-text="$t('actions.verify')"
            />
          </div>
        </control-pin-field>
      </form>
      <div v-else v-text="$t('settings.devices.no-active-pairing')" />
    </template>
  </content-with-heading>
  <content-with-heading>
    <template #heading>
      <pane-title
        :content="{ title: $t('settings.devices.speaker-pairing') }"
      />
    </template>
    <template #content>
      <div
        class="content"
        v-text="$t('settings.devices.speaker-pairing-info')"
      />
      <div
        class="content"
        v-text="$t('settings.devices.speaker-test-info')"
      />
      <div
        v-for="output in outputsStore.outputs"
        :key="output.id"
        class="field columns is-multiline"
      >
        <div class="column is-flex is-one-third">
          <control-switch
            v-model="output.selected"
            @update:model-value="toggleOutput(output.id)"
          >
            <template #label>
              <span v-text="output.name" />
            </template>
          </control-switch>
        </div>
        <div class="column is-one-third">
          <control-integer-field
            v-model="output.offset_ms"
            :min="-2000"
            :max="2000"
            :step="50"
            @update:model-value="
              (value) => onOutputOffsetChange(output.id, value)
            "
          >
            <mdicon class="icon is-small is-left" name="timelapse" size="16" />
            <span class="icon is-right">ms</span>
          </control-integer-field>
        </div>
        <div class="column is-one-third">
          <button
            class="button is-small mb-2"
            :class="{ 'is-loading': testRunning && testId === output.id }"
            :disabled="testRunning"
            @click="testOutput(output.id)"
          >
            <mdicon class="icon" name="volume-high" size="16" />
            <span v-text="$t('settings.devices.speaker-test')" />
          </button>
          <p
            v-if="testId === output.id && testMessage"
            class="help"
            :class="testClass"
            v-text="testMessage"
          />
          <form
            v-if="output.needs_auth_key"
            @submit.prevent="pairOutput(output.id)"
          >
            <control-pin-field
              class="has-addons"
              :placeholder="$t('settings.devices.verification-code')"
              @input="onOutputPinChange"
            >
              <div class="control">
                <button
                  class="button"
                  type="submit"
                  v-text="$t('actions.verify')"
                />
              </div>
            </control-pin-field>
          </form>
        </div>
      </div>
    </template>
  </content-with-heading>
</template>

<script setup>
import { computed, ref } from 'vue'
import ContentWithHeading from '@/templates/ContentWithHeading.vue'
import ControlIntegerField from '@/components/ControlIntegerField.vue'
import ControlPinField from '@/components/ControlPinField.vue'
import ControlSwitch from '@/components/ControlSwitch.vue'
import PaneTitle from '@/components/PaneTitle.vue'
import TabsSettings from '@/components/TabsSettings.vue'
import outputs from '@/api/outputs'
import remotes from '@/api/remotes'
import { useI18n } from 'vue-i18n'
import { useOutputsStore } from '@/stores/outputs'
import { useRemotesStore } from '@/stores/remotes'

const { t } = useI18n()
const outputsStore = useOutputsStore()
const remotesStore = useRemotesStore()

const outputPin = ref('')
const testId = ref('')
const testMessage = ref('')
const testRunning = ref(false)
const testState = ref('')
const remotePairingDisabled = ref(true)
const remotePin = ref('')

const onOutputPinChange = (pin) => {
  outputPin.value = pin
}

const onOutputOffsetChange = (identifier, value) => {
  outputs.update(identifier, { offset_ms: value })
}

const onRemotePinChange = (pin, disabled) => {
  remotePin.value = pin
  remotePairingDisabled.value = disabled
}

const pairOutput = (identifier) => {
  outputs.update(identifier, { pin: outputPin.value })
}

const pairRemote = () => {
  remotes.pair(remotePin.value)
}

const toggleOutput = (identifier) => {
  outputs.toggle(identifier)
}

const TEST_POLL_MS = 500

const testClass = computed(() => {
  if (testState.value === 'passed') {
    return 'is-success'
  }
  if (testState.value === 'failed') {
    return 'is-danger'
  }
  return ''
})

const showTest = (status) => {
  testState.value = status.state
  testMessage.value = status.reason || status.message
}

const pollTest = () => {
  outputs.testStatus().then((status) => {
    showTest(status)
    if (status.state === 'running') {
      setTimeout(pollTest, TEST_POLL_MS)
    } else {
      testRunning.value = false
    }
  })
}

const testOutput = (identifier) => {
  testId.value = identifier
  testMessage.value = t('settings.devices.speaker-test-running')
  testState.value = 'running'
  testRunning.value = true
  outputs
    .test(identifier)
    .then((status) => {
      showTest(status)
      if (status.started) {
        setTimeout(pollTest, TEST_POLL_MS)
      } else {
        testState.value = 'failed'
        testRunning.value = false
      }
    })
    .catch(() => {
      testRunning.value = false
    })
}
</script>
