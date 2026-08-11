# assets

Empty until the phases that need them.

- `audio/` — short WAV/ADPCM effects and jingles (Phase 8): alarm tone,
  step-done blip, routine-complete fanfare, XP chime. Optional pre-rendered
  Greek/English voice prompts. **No MP3** — there is no decoder in the build.
- `fonts/` — `lv_font_conv` output for Noto Greek at the sizes the UI uses.
  Greek (el) is a first-class locale, not a fallback.
- `emoji/` — generated from the app repo's `stepIcons.ts` via
  `tools/watch-emoji/gen-watch-emoji.mjs` so the watch, tablet and parent app
  never drift. ~4MB; lives in the littlefs partition.

There is **no microSD slot** on this board — everything ships in the 16MB flash.
