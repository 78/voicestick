<script setup>
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { ESPLoader, Transport } from 'esptool-js'
import { setLocale } from './i18n'
import productPhoto from './assets/sticks3.png'
import packageInfo from '../package.json'

const { locale, t } = useI18n()
const githubUrl = 'https://github.com/78/voicestick'
const releaseUrl = `${githubUrl}/releases`
const macVersion = import.meta.env.VITE_RELEASE_VERSION || packageInfo.version
const windowsVersion = import.meta.env.VITE_WINDOWS_RELEASE_VERSION || packageInfo.version
const macDownloadUrl = import.meta.env.VITE_MAC_DOWNLOAD_URL || `${releaseUrl}/download/v${macVersion}/VoiceStick-${macVersion}.dmg`
const windowsDownloadUrl = import.meta.env.VITE_WINDOWS_DOWNLOAD_URL || `${releaseUrl}/download/v${windowsVersion}/VoiceStick_${windowsVersion}.msi`
const firmwareBase = 'https://xiaozhi-voice-assistant.oss-cn-shenzhen.aliyuncs.com/voicestick/firmwares'
const boards = {
  stick_s3: {
    name: 'M5Stack StickS3',
    chip: 'ESP32-S3',
    manifestUrl: import.meta.env.VITE_FIRMWARE_MANIFEST_URL || `${firmwareBase}/latest/manifest.json`,
    flashFreq: '80m',
  },
  voicestick_c3: {
    name: 'VoiceStick C3 v1',
    chip: 'ESP32-C3',
    manifestUrl: import.meta.env.VITE_C3_FIRMWARE_MANIFEST_URL || `${firmwareBase}/voicestick_c3/latest/manifest.json`,
    flashFreq: 'keep',
  },
}
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
const selectedBoardID = ref('stick_s3')
const selectedBoard = computed(() => boards[selectedBoardID.value])
const manifests = ref({})
const manifestRequests = new Map()
const firmwareManifest = computed(() => manifests.value[selectedBoardID.value])

watch(selectedBoardID, (boardID) => {
  flashStatus.value = 'idle'
  flashProgress.value = 0
  flashLog.value = []
  connectedChip.value = ''
  loadFirmwareManifest(boardID).catch(() => {
    manifests.value[boardID] = null
  })
}, { immediate: true })

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

async function fetchFirmware(boardID) {
  flashStatus.value = 'downloading'
  appendLog(t('flasher.log.downloading'))

  const manifest = await loadFirmwareManifest(boardID)
  const response = await fetch(manifest.merged_url, { cache: 'no-store' })
  if (!response.ok) {
    throw new Error(t('flasher.error.downloadFailed', { status: response.status }))
  }

  const firmware = new Uint8Array(await response.arrayBuffer())
  appendLog(t('flasher.log.downloaded', { size: formatBytes(firmware.byteLength) }))
  return firmware
}

function loadFirmwareManifest(boardID) {
  const cached = manifests.value[boardID]
  if (cached) {
    return Promise.resolve(cached)
  }
  if (!manifestRequests.has(boardID)) {
    const request = fetch(boards[boardID].manifestUrl, { cache: 'no-store' })
      .then(async (response) => {
        if (!response.ok) {
          throw new Error()
        }
        const manifest = await response.json()
        if (manifest?.hardware !== boardID || !manifest.version || !manifest.merged_url) {
          throw new Error()
        }
        manifests.value[boardID] = manifest
        return manifest
      })
      .catch(() => {
        throw new Error(t('flasher.error.manifestUnavailable'))
      })
      .finally(() => manifestRequests.delete(boardID))
    manifestRequests.set(boardID, request)
  }
  return manifestRequests.get(boardID)
}

async function flashFirmware() {
  if (!serialSupported.value) {
    flashStatus.value = 'unsupported'
    return
  }

  let transport
  const boardID = selectedBoardID.value
  const board = boards[boardID]

  try {
    flashing.value = true
    flashStatus.value = 'connecting'
    flashProgress.value = 0
    connectedChip.value = ''
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

    if (loader.chip.CHIP_NAME !== board.chip) {
      throw new Error(t('flasher.error.wrongChip', { chip, board: board.name, expected: board.chip }))
    }

    const firmware = await fetchFirmware(boardID)

    flashStatus.value = 'flashing'
    appendLog(t('flasher.log.flashing'))
    await loader.writeFlash({
      fileArray: [{ data: firmware, address: 0x0 }],
      flashMode: 'dio',
      flashFreq: board.flashFreq,
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
    if (transport) {
      try {
        await transport.disconnect()
      } catch {
        // The device may already have reset and closed the stream.
      }
    }
    flashing.value = false
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
          <a class="button primary mac" :href="macDownloadUrl">
            {{ t('hero.downloadMac') }} <span class="download-version">{{ macVersion }}</span>
          </a>
          <a class="button primary windows" :href="windowsDownloadUrl">
            {{ t('hero.downloadWindows') }} <span class="download-version">{{ windowsVersion }}</span>
          </a>
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
          <h2>{{ t('flasher.title') }}</h2>
          <p>{{ t('flasher.body') }}</p>
          <ol class="flash-steps">
            <li>{{ t('flasher.steps.usb', { board: selectedBoard.name }) }}</li>
            <li>{{ t('flasher.steps.flash') }}</li>
            <li>{{ t(`flasher.boot.${selectedBoardID}`) }}</li>
          </ol>
        </div>

        <div class="flasher-panel">
          <div class="flasher-board">
            <label for="flash-board">{{ t('flasher.board') }}</label>
            <select id="flash-board" v-model="selectedBoardID" :disabled="flashing">
              <option v-for="(board, id) in boards" :key="id" :value="id">{{ board.name }}</option>
            </select>
          </div>
          <div class="flasher-meta">
            <span role="status">{{ t(`flasher.status.${flashStatus}`) }}</span>
            <span>{{ connectedChip || selectedBoard.chip }}</span>
          </div>

          <div class="progress-track" aria-hidden="true">
            <span :style="{ width: `${flashProgress}%` }"></span>
          </div>

          <div class="flasher-actions">
            <button class="button primary" type="button" :disabled="!canFlash" @click="flashFirmware">
              {{ flashing ? t('flasher.button.flashing') : t('flasher.button.start') }}
            </button>
            <a v-if="firmwareManifest" class="button secondary" :href="firmwareManifest.merged_url">
              {{ t('flasher.button.download') }}
            </a>
            <button v-else class="button secondary" type="button" disabled>{{ t('flasher.button.download') }}</button>
          </div>

          <p v-if="!serialSupported" class="browser-warning">{{ t('flasher.unsupported') }}</p>
          <p v-if="firmwareManifest" class="firmware-note">{{ t('flasher.firmware.version', { version: firmwareManifest.version }) }}</p>
          <p v-else-if="firmwareManifest === null" class="firmware-note">{{ t('flasher.error.manifestUnavailable') }}</p>

          <div class="flash-log">
            <p v-if="flashLog.length === 0">{{ t('flasher.log.empty') }}</p>
            <p v-for="(line, index) in flashLog" :key="`${index}-${line}`">{{ line }}</p>
          </div>
        </div>
      </div>
    </section>

    <section class="download-band" id="download">
      <div class="section-inner download-inner">
        <div>
          <h2>{{ t('download.title') }}</h2>
          <p>{{ t('download.body') }}</p>
        </div>
        <a class="button secondary" :href="releaseUrl">{{ t('download.cta') }}</a>
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
