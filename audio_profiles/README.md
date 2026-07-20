# Audio profiles

`pcm5102-a1s.json` keeps the PCM5102 output close to the ESP32-A1S volume
curve and applies a small, independent bass trim.

After changing a value, regenerate the firmware header:

```sh
python3 scripts/apply_audio_profile.py audio_profiles/pcm5102-a1s.json
```

Then rebuild normally. Keep this folder, the generator, and the
`audio_output_profile` module when rebasing onto a newer firmware release.

- `output_*_db` controls the volume curve.
- `bass_shelf_db` controls bass gain; `0` disables tone correction.
- `bass_shelf_hz` controls the upper edge of the bass adjustment.
