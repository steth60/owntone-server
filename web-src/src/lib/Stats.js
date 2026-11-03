// Helpers for the performance page, which polls /api/stats

const KB_PER_MB = 1024
const KB_PER_GB = 1024 * 1024
const MINUTE = 60
const HOUR = 3600
const DAY = 86400
const MS_PER_S = 1000

export const intervalSeconds = (current, previous) => {
  if (!current || !previous) {
    return 0
  }
  return (current.now_ms - previous.now_ms) / MS_PER_S
}

// Per second increase of a counter between two snapshots
export const rate = (now, before, seconds) => {
  if (!seconds || typeof before !== 'number') {
    return 0
  }
  return Math.max(0, (now - before) / seconds)
}

export const cpuPercent = (current, previous) => {
  const seconds = intervalSeconds(current, previous)
  if (!seconds) {
    return null
  }
  const used = current.process.cpu_time_ms - previous.process.cpu_time_ms
  return (used / (seconds * MS_PER_S)) * 100
}

export const formatKb = (kb) => {
  if (typeof kb !== 'number') {
    return '–'
  }
  if (kb >= KB_PER_GB) {
    return `${(kb / KB_PER_GB).toFixed(1)} GB`
  }
  if (kb >= KB_PER_MB) {
    return `${(kb / KB_PER_MB).toFixed(1)} MB`
  }
  return `${Math.round(kb)} KB`
}

export const formatDuration = (s) => {
  if (typeof s !== 'number') {
    return '–'
  }
  if (s < MINUTE) {
    return `${s} s`
  }
  if (s < HOUR) {
    return `${Math.floor(s / MINUTE)} min`
  }
  if (s < DAY) {
    return `${Math.floor(s / HOUR)} h ${Math.floor((s % HOUR) / MINUTE)} min`
  }
  return `${Math.floor(s / DAY)} d ${Math.floor((s % DAY) / HOUR)} h`
}

const rateText = (packets, bytes) => {
  if (packets <= 0) {
    return '–'
  }
  return `${packets.toFixed(0)} pkt/s · ${((bytes * 8) / MS_PER_S).toFixed(0)} kbit/s`
}

const eventText = (o) => {
  if (!o.last_event) {
    return ''
  }
  return `${o.last_event} (${formatDuration(o.last_event_age_s)} ago)`
}

const speakerRow = (speaker, { o, b }, seconds) => {
  const resent = rate(o.retransmits ?? 0, b.retransmits, seconds)
  return {
    clipping: rate(o.eq_clips ?? 0, b.eq_clips, seconds) > 0,
    eq: Boolean(o.eq_active),
    event: eventText(o),
    failures: o.failures ?? 0,
    failuresIncreased: (o.failures ?? 0) > (b.failures ?? o.failures ?? 0),
    id: speaker.id,
    lost: rate(o.retransmits_missed ?? 0, b.retransmits_missed, seconds) > 0,
    missed: o.retransmits_missed ?? 0,
    name: speaker.name,
    rateText: rateText(
      rate(o.packets ?? 0, b.packets, seconds),
      rate(o.bytes ?? 0, b.bytes, seconds)
    ),
    resending: resent > 0,
    retransmits: o.retransmits ?? 0,
    selected: speaker.selected,
    transport: o.transport || '–'
  }
}

// Speakers that are selected or have been used, with their counters
export const speakerRows = (current, previous) => {
  const outputs = current?.outputs ?? {}
  const before = previous?.outputs ?? {}
  const seconds = intervalSeconds(current, previous)
  return (current?.speakers ?? [])
    .filter((speaker) => speaker.selected || outputs[speaker.id])
    .map((speaker) =>
      speakerRow(
        speaker,
        { b: before[speaker.id] ?? {}, o: outputs[speaker.id] ?? {} },
        seconds
      )
    )
}

export const counterIncreased = (snapshots, section, key) =>
  Boolean(snapshots.previous) &&
  snapshots.current[section][key] > snapshots.previous[section][key]
