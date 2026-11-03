// Speaker EQ profiles as used by the /api/outputs/{id}/eq endpoint

export const BAND_FREQS = [60, 230, 910, 3600, 14000]
export const CHANNELS = ['stereo', 'left', 'right', 'mono', 'swap']
export const CROSSOVER_OPTIONS = [
  0, 40, 60, 80, 100, 120, 150, 200, 300, 500, 1000
]
export const PRESETS = [
  'flat',
  'bass-boost',
  'bass-only',
  'side-left',
  'side-right',
  'off'
]

export const flatProfile = (enabled = true) => ({
  bands: BAND_FREQS.map((freq) => ({ freq, gain_db: 0 })),
  channels: 'stereo',
  enabled,
  highpass_hz: 0,
  lowpass_hz: 0,
  preamp_db: 0
})

const PRESET_CHANGES = {
  'bass-boost': (profile) => {
    profile.bands[0].gain_db = 6
    profile.bands[1].gain_db = 2
    profile.preamp_db = -6
  },
  'bass-only': (profile) => {
    profile.lowpass_hz = 120
  },
  'side-left': (profile) => {
    profile.highpass_hz = 120
    profile.channels = 'left'
  },
  'side-right': (profile) => {
    profile.highpass_hz = 120
    profile.channels = 'right'
  }
}

export const presetProfile = (key) => {
  const profile = flatProfile(key !== 'off')
  PRESET_CHANGES[key]?.(profile)
  return profile
}

export const crossoverInvalid = (profile) =>
  profile.highpass_hz > 0 &&
  profile.lowpass_hz > 0 &&
  profile.highpass_hz >= profile.lowpass_hz

export const formatGain = (db) => {
  const value = Number(db).toFixed(1)
  if (db > 0) {
    return `+${value} dB`
  }
  return `${value} dB`
}

export const formatFreq = (hz) => {
  if (hz >= 1000) {
    return `${hz / 1000} kHz`
  }
  return `${hz} Hz`
}
