<script setup>
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { ESPLoader, Transport } from 'esptool-js'
import { setLocale } from './i18n'
import productPhoto from './assets/sticks3.png'
import packageInfo from '../package.json'

const { locale, t } = useI18n()
const releaseUrl = 'https://github.com/78/voicestick/releases/latest'
const githubUrl = 'https://github.com/78/voicestick'
const releaseDownloadBase = `https://github.com/78/voicestick/releases/download/v${packageInfo.version}`
const macDownloadUrl = `${releaseDownloadBase}/VoiceStick-${packageInfo.version}.dmg`
const windowsDownloadUrl = `${releaseDownloadBase}/VoiceStick_${packageInfo.version}.msi`
const defaultFirmwareUrl = `https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/voicestick-firmware-sticks3-merged-${packageInfo.version}.bin`
const firmwareManifestUrl = import.meta.env.VITE_FIRMWARE_MANIFEST_URL || 'https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares/latest/manifest.json'
const firmwareUrl = ref(import.meta.env.VITE_FIRMWARE_URL || defaultFirmwareUrl)
const appResetSequence = 'D0|R1|W100|R0|W500|D0'

const languageLabel = computed(() => (locale.value === 'zh-CN' ? t('language.en') : t('language.zh')))
const nextLocale = computed(() => (locale.value === 'zh-CN' ? 'en-US' : 'zh-CN'))
const serialSupported = computed(() => typeof navigator !== 'undefined' && 'serial' in navigator)
const canFlash = computed(() => serialSupported.value && !flashing.value)

const flashing = ref(false)
const flashStatus = ref('idle')
const flashProgress = ref(0)
const flashLog = ref([])
const connectedChip = ref('')
const firmwareSize = ref('')

const terminal = {
  clean() {
    flashLog.value = []
  },
  write(data) {
    appendLog(data)
  },
  writeLine(data) {
    appendLog(data)
  },
}

function toggleLanguage() {
  setLocale(nextLocale.value)
}

function appendLog(data) {
  const lines = String(data)
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)

  if (lines.length === 0) {
    return
  }

  flashLog.value = [...flashLog.value, ...lines].slice(-12)
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return ''
  }

  if (bytes < 1024 * 1024) {
    return `${Math.round(bytes / 1024)} KB`
  }

  return `${(bytes / 1024 / 1024).toFixed(1)} MB`
}

async function fetchFirmware() {
  flashStatus.value = 'downloading'
  appendLog(t('flasher.log.downloading'))

  await resolveFirmwareUrl()
  const response = await fetch(firmwareUrl.value, { cache: 'no-store' })
  if (!response.ok) {
    throw new Error(t('flasher.error.downloadFailed', { status: response.status }))
  }

  const firmware = new Uint8Array(await response.arrayBuffer())
  firmwareSize.value = formatBytes(firmware.byteLength)
  appendLog(t('flasher.log.downloaded', { size: firmwareSize.value }))
  return firmware
}

async function resolveFirmwareUrl() {
  if (!firmwareManifestUrl) {
    return
  }

  const response = await fetch(firmwareManifestUrl, { cache: 'no-store' })
  if (!response.ok) {
    return
  }

  const manifest = await response.json()
  if (manifest?.merged_url) {
    firmwareUrl.value = manifest.merged_url
  }
}

async function flashFirmware() {
  if (!serialSupported.value) {
    flashStatus.value = 'unsupported'
    return
  }

  let transport

  try {
    flashing.value = true
    flashStatus.value = 'connecting'
    flashProgress.value = 0
    connectedChip.value = ''
    firmwareSize.value = ''
    flashLog.value = []
    appendLog(t('flasher.log.selectPort'))

    const port = await navigator.serial.requestPort()
    transport = new Transport(port, false)
    const loader = new ESPLoader({
      transport,
      baudrate: 460800,
      terminal,
      debugLogging: false,
    })

    appendLog(t('flasher.log.connecting'))
    const chip = await loader.main('default_reset')
    connectedChip.value = chip

    if (!chip.toLowerCase().includes('esp32-s3')) {
      throw new Error(t('flasher.error.wrongChip', { chip }))
    }

    const firmware = await fetchFirmware()

    flashStatus.value = 'flashing'
    appendLog(t('flasher.log.flashing'))
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: 0x0 }],
      flashMode: 'dio',
      flashFreq: '80m',
      flashSize: '8MB',
      eraseAll: false,
      compress: true,
      reportProgress: (_fileIndex, written, total) => {
        flashProgress.value = Math.min(100, Math.round((written / total) * 100))
      },
    })

    flashProgress.value = 100
    flashStatus.value = 'resetting'
    await loader.after('custom_reset', undefined, appResetSequence)
    flashStatus.value = 'done'
    appendLog(t('flasher.log.done'))
  } catch (error) {
    flashStatus.value = error?.name === 'NotFoundError' ? 'idle' : 'error'
    if (error?.name !== 'NotFoundError') {
      appendLog(error?.message || String(error))
    }
  } finally {
    flashing.value = false
    if (transport) {
      try {
        await transport.disconnect()
      } catch {
        // The device may already have reset and closed the stream.
      }
    }
  }
}
</script>

<template>
  <header class="topbar">
    <div class="topbar-inner">
      <a class="brand" href="./" aria-label="VoiceStick">
        <span class="brand-mark" aria-hidden="true"></span>
        <span>VoiceStick</span>
      </a>
      <nav>
        <a href="#flash">{{ t('nav.flash') }}</a>
        <a href="#download">{{ t('nav.download') }}</a>
        <a :href="githubUrl">{{ t('nav.github') }}</a>
        <button class="language-button" type="button" :aria-label="t('language.label')" @click="toggleLanguage">
          {{ languageLabel }}
        </button>
      </nav>
    </div>
  </header>

  <main>
    <section class="hero">
      <div class="hero-copy">
        <p class="eyebrow">{{ t('hero.eyebrow') }}</p>
        <h1>{{ t('hero.title') }}</h1>
        <p class="lead">{{ t('hero.lead') }}</p>
        <div class="actions">
          <a class="button primary" href="#download">{{ t('hero.download') }}</a>
          <a class="button secondary" href="#flash">{{ t('hero.flash') }}</a>
          <a class="button secondary" :href="githubUrl">{{ t('hero.source') }}</a>
        </div>
      </div>
      <div class="product-visual" :aria-label="t('hero.imageAlt')">
        <img class="product-photo" :src="productPhoto" :alt="t('hero.imageAlt')">
      </div>
    </section>

    <section class="features" :aria-label="t('features.label')">
      <div class="section-inner features-grid">
        <article>
          <h2>{{ t('features.ble.title') }}</h2>
          <p>{{ t('features.ble.body') }}</p>
        </article>
        <article>
          <h2>{{ t('features.model.title') }}</h2>
          <p>{{ t('features.model.body') }}</p>
        </article>
        <article>
          <h2>{{ t('features.feedback.title') }}</h2>
          <p>{{ t('features.feedback.body') }}</p>
        </article>
      </div>
    </section>

    <section class="story">
      <div class="section-inner story-grid">
        <div>
          <p class="eyebrow">{{ t('story.eyebrow') }}</p>
          <h2>{{ t('story.title') }}</h2>
        </div>
        <p>{{ t('story.body') }}</p>
      </div>
    </section>

    <section class="flasher" id="flash">
      <div class="section-inner flasher-grid">
        <div class="flasher-copy">
          <p class="eyebrow">{{ t('flasher.eyebrow') }}</p>
          <h2>{{ t('flasher.title') }}</h2>
          <p>{{ t('flasher.body') }}</p>
          <ol class="flash-steps">
            <li>{{ t('flasher.steps.usb') }}</li>
            <li>{{ t('flasher.steps.boot') }}</li>
            <li>{{ t('flasher.steps.flash') }}</li>
          </ol>
        </div>

        <div class="flasher-panel" aria-live="polite">
          <div class="flasher-meta">
            <span>{{ t(`flasher.status.${flashStatus}`) }}</span>
            <span v-if="connectedChip">{{ connectedChip }}</span>
            <span v-else>{{ t('flasher.meta.target') }}</span>
          </div>

          <div class="progress-track" aria-hidden="true">
            <span :style="{ width: `${flashProgress}%` }"></span>
          </div>

          <div class="flasher-actions">
            <button class="button primary" type="button" :disabled="!canFlash" @click="flashFirmware">
              {{ flashing ? t('flasher.button.flashing') : t('flasher.button.start') }}
            </button>
            <a class="button secondary" :href="firmwareUrl">{{ t('flasher.button.download') }}</a>
          </div>

          <p v-if="!serialSupported" class="browser-warning">{{ t('flasher.unsupported') }}</p>
          <p v-else class="firmware-note">
            {{ firmwareSize ? t('flasher.firmware.ready', { size: firmwareSize }) : t('flasher.firmware.latest') }}
          </p>

          <div class="flash-log">
            <p v-if="flashLog.length === 0">{{ t('flasher.log.empty') }}</p>
            <p v-for="(line, index) in flashLog" :key="`${index}-${line}`">{{ line }}</p>
          </div>
        </div>
      </div>
    </section>

    <section class="download-band" id="download">
      <div class="section-inner download-inner">
        <div class="download-copy">
          <h2>{{ t('download.title') }}</h2>
          <p>{{ t('download.body') }}</p>
        </div>
        <div class="download-options">
          <article class="download-card">
            <div>
              <span class="platform-label">{{ t('download.mac.platform') }}</span>
              <h3>{{ t('download.mac.title') }}</h3>
              <p>{{ t('download.mac.body') }}</p>
            </div>
            <a class="button primary" :href="macDownloadUrl">{{ t('download.mac.cta') }}</a>
          </article>
          <article class="download-card">
            <div>
              <span class="platform-label">{{ t('download.windows.platform') }}</span>
              <h3>{{ t('download.windows.title') }}</h3>
              <p>{{ t('download.windows.body') }}</p>
            </div>
            <a class="button primary" :href="windowsDownloadUrl">{{ t('download.windows.cta') }}</a>
          </article>
          <a class="release-link" :href="releaseUrl">{{ t('download.allReleases') }}</a>
        </div>
      </div>
    </section>
  </main>

  <footer>
    <div class="section-inner footer-inner">
      <span>VoiceStick</span>
      <a href="./appcast.xml">{{ t('footer.appcast') }}</a>
    </div>
  </footer>
</template>
