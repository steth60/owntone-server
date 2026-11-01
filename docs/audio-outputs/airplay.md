# AirPlay devices/speakers

OwnTone will discover the AirPlay devices available on your network. For
devices that are password-protected, the device's AirPlay name and password
must be given in the configuration file. See the sample configuration file
for the syntax.

If your Apple TV requires device verification (always required by Apple TV4 with
tvOS 10.2) then you can do that through Settings > Remotes & Outputs in the web
interface: Select the device and then enter the PIN that the Apple TV displays.

If your speaker is silent when you start playback, and there is no obvious error
message in the log, you can try disabling ipv6 in the config. Some speakers
announce that they support ipv6, but for some reason don't work with OwnTone.

If the speaker becomes unselected when you start playback, and you in the log
see "ANNOUNCE request failed in session startup: 400 Bad Request", then try
the Apple Home app > Allow Speakers & TV Access > Anyone On the Same Network
(or Everyone).

## Buffered audio (lower latency)

AirPlay 2 has two ways of streaming audio. By default OwnTone uses the
"realtime" stream (ALAC over UDP), which has a fixed latency of about two
seconds. Most AirPlay 2 devices, including HomePods and Apple TVs, also support
"buffered" audio (AAC over TCP), where the device plays what it receives at the
exact time OwnTone asks it to. With buffered audio the latency is governed by
`start_buffer_ms` in the configuration, so playback can start noticeably faster.

Buffered audio is opt-in per speaker. In the web interface, or via the JSON API,
set the speaker's format to `aac` (the default is `alac`). The option is only
offered for devices that advertise support for buffered audio and PTP. Note
that the buffered stream is AAC, which is lossy, whereas the realtime stream is
lossless. The change takes effect the next time the speaker is started.
