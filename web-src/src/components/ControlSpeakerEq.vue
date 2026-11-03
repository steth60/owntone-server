<template>
  <div class="box">
    <div
      class="is-flex is-justify-content-space-between is-align-items-center is-flex-wrap-wrap mb-3"
    >
      <control-switch v-model="profile.enabled" @update:model-value="save">
        <template #label>
          <b v-text="output.name" />
          <span
            v-if="output.selected"
            class="tag is-info is-light ml-2"
            v-text="$t('settings.eq.playing')"
          />
          <span
            v-if="saved"
            class="tag is-success is-light ml-2"
            v-text="$t('settings.eq.saved')"
          />
        </template>
      </control-switch>
      <div class="select is-small">
        <select :aria-label="$t('settings.eq.presets')" @change="onPreset">
          <option value="" v-text="$t('settings.eq.presets')" />
          <option
            v-for="preset in PRESETS"
            :key="preset"
            :value="preset"
            v-text="$t(`settings.eq.preset-${preset}`)"
          />
        </select>
      </div>
    </div>
    <div :class="{ 'eq-disabled': !profile.enabled }">
      <div class="eq-sliders">
        <div class="eq-band">
          <input
            v-model.number="profile.preamp_db"
            class="eq-slider"
            type="range"
            min="-24"
            max="12"
            step="0.5"
            :aria-label="$t('settings.eq.preamp')"
            @input="queueSave"
          />
          <div class="is-size-7" v-text="formatGain(profile.preamp_db)" />
          <div
            class="is-size-7 has-text-grey"
            v-text="$t('settings.eq.preamp')"
          />
        </div>
        <div class="eq-divider" />
        <div
          v-for="(band, index) in profile.bands"
          :key="index"
          class="eq-band"
        >
          <input
            v-model.number="band.gain_db"
            class="eq-slider"
            type="range"
            min="-12"
            max="12"
            step="0.5"
            :aria-label="formatFreq(band.freq)"
            @input="queueSave"
            @dblclick="resetBand(index)"
          />
          <div class="is-size-7" v-text="formatGain(band.gain_db)" />
          <div class="is-size-7 has-text-grey" v-text="formatFreq(band.freq)" />
        </div>
      </div>
      <div class="columns is-multiline mt-2">
        <div
          v-for="field in ['highpass_hz', 'lowpass_hz']"
          :key="field"
          class="column is-one-third"
        >
          <label
            class="label is-small"
            v-text="$t(`settings.eq.${field.replace('_hz', '')}`)"
          />
          <div class="select is-small is-fullwidth">
            <select v-model.number="profile[field]" @change="save">
              <option
                v-for="hz in CROSSOVER_OPTIONS"
                :key="hz"
                :value="hz"
                v-text="hz ? `${hz} Hz` : $t('settings.eq.off')"
              />
            </select>
          </div>
        </div>
        <div class="column is-one-third">
          <label class="label is-small" v-text="$t('settings.eq.channels')" />
          <div class="select is-small is-fullwidth">
            <select v-model="profile.channels" @change="save">
              <option
                v-for="channel in CHANNELS"
                :key="channel"
                :value="channel"
                v-text="$t(`settings.eq.channel-${channel}`)"
              />
            </select>
          </div>
        </div>
      </div>
      <p
        v-if="crossoverInvalid(profile)"
        class="help is-danger"
        v-text="`${$t('settings.eq.highpass')} ≥ ${$t('settings.eq.lowpass')}`"
      />
    </div>
  </div>
</template>

<script setup>
import {
  CHANNELS,
  CROSSOVER_OPTIONS,
  PRESETS,
  crossoverInvalid,
  flatProfile,
  formatFreq,
  formatGain,
  presetProfile
} from '@/lib/Equaliser'
import { onBeforeUnmount, onMounted, ref } from 'vue'
import ControlSwitch from '@/components/ControlSwitch.vue'
import outputs from '@/api/outputs'

const SAVE_DELAY_MS = 250
const SAVED_SHOWN_MS = 1500

const props = defineProps({ output: { required: true, type: Object } })

const profile = ref(flatProfile(false))
const saved = ref(false)
let saveTimer = null
let savedTimer = null

onMounted(async () => {
  try {
    profile.value = {
      ...flatProfile(false),
      ...(await outputs.eq(props.output.id))
    }
  } catch {
    // Keep the flat profile, the interceptor shows the error
  }
})

const save = async () => {
  if (crossoverInvalid(profile.value)) {
    return
  }
  await outputs.setEq(props.output.id, profile.value)
  saved.value = true
  clearTimeout(savedTimer)
  savedTimer = setTimeout(() => {
    saved.value = false
  }, SAVED_SHOWN_MS)
}

// Sliders send many events, only save when they settle
const queueSave = () => {
  clearTimeout(saveTimer)
  saveTimer = setTimeout(save, SAVE_DELAY_MS)
}

const resetBand = (index) => {
  profile.value.bands[index].gain_db = 0
  save()
}

const onPreset = (event) => {
  const key = event.target.value
  event.target.value = ''
  if (key) {
    profile.value = presetProfile(key)
    save()
  }
}

onBeforeUnmount(() => {
  clearTimeout(saveTimer)
  clearTimeout(savedTimer)
})
</script>

<style scoped>
.eq-sliders {
  display: flex;
  flex-wrap: wrap;
  gap: 0.75rem;
  align-items: flex-end;
}
.eq-band {
  display: flex;
  flex-direction: column;
  align-items: center;
  min-width: 3.5rem;
}
.eq-slider {
  writing-mode: vertical-lr;
  direction: rtl;
  height: 8rem;
  width: 1.5rem;
  cursor: pointer;
}
.eq-divider {
  width: 1px;
  align-self: stretch;
  background: var(--bulma-border);
}
.eq-disabled {
  opacity: 0.45;
}
</style>
