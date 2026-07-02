# The corpus: reference captures and their quirks

Lossless real-int16 RF/IF masters under `corpus/` (git LFS), each a SigMF pair;
the capture chains and conventions are in the top-level README. All four come
from the same source: a Sega Master System II over UK PAL RF, so they share the
console's quirks - which several defaults and tests are calibrated against.

## The clips

| clip | SDR / rate | content |
|---|---|---|
| `alex_kidd` | RX888, 32 MS/s | Alex Kidd in Miracle World, gameplay |
| `alex_kidd_title` | RX888, 32 MS/s | Alex Kidd title screen (static) |
| `wb3` | RX888, 32 MS/s | Wonder Boy III: The Dragon's Trap, gameplay |
| `wb3_airspy` | AirSpy R2, 20 MS/s raw real | Wonder Boy III, gameplay |

Carriers are absolute IF frequencies in the `rx888:*` / `airspy:*` metadata;
`tools/inspect_capture.py` is the pre-decode QC.

## Load-bearing quirks (things calibrated against this corpus)

- **The SMS line rate runs off-nominal**: ~0.35 us long per 64 us line
  (~15520 Hz vs the nominal 15625). This is what makes `--comb-mode glass`
  (the fixed 63.943 us block) visibly misregister, exactly as a real PAL-D set
  would - the adaptive `delay-line`/`post` modes track it.
- **The SMS RF modulator under-modulates**: white reaches only ~50% of the
  carrier where broadcast geometry puts it at 20% (i.e. much shallower picture
  depth), so a period set shows it dim. The shipped `--contrast 1.6` default is
  a provisional calibration to this corpus (issue #46 tracks level-setting it
  against more sources); broadcast-standard modulation wants ~1.0. The
  modulation *geometry* (sync/blanking ratios) is standard.
- **Clips are sub-second** (~0.5-1 s), shorter than some of the decoder's
  authentic time constants: the colour killer's switch-on ramp (~0.1 s) spans a
  visible fraction of a clip, so looped playback shows the saturation swell and
  snap at the seam - power-on behaviour, not a bug.
- The SMS crystal sits ~3 Hz low of the textbook 4.43361875 MHz; both SDRs
  agree via the APC pull diagnostic (`render --colour` prints it).
