# Embedded Root CA Bundle

HTTPS notification targets (`webhook_url` and `local_url` when configured with `https://`) verify server certificates through `WiFiClientSecure::setCACertBundle()` against the binary bundle embedded from `firmware/data/cert/x509_crt_bundle.bin`.

## Provenance

- **Bundle file:** `firmware/data/cert/x509_crt_bundle.bin` (`55587` bytes, `121` root certificates)
- **Bundle SHA-256:** `49e7e1ca53f48330b1b507872f1447eb5f333632b6802282ec51aaab5640787c`
- **Root certificate list:** Mozilla CA certificate list shipped as `cacert.pem` with Python `certifi` version `2026.07.22` (`240216` bytes)
- **Source `cacert.pem` SHA-256:** `9cc2a774b5198dcff14d9be1e66091f538975d867ce029a96bce15a55dfd730f`
- **Licence of the root list:** Mozilla Public License 2.0 (MPL-2.0); the source form is the `cacert.pem` above.
- **Generator:** `gen_crt_bundle.py` from Espressif ESP-IDF (Apache License 2.0), not part of this repository and not needed to build the firmware:
  - Pinned URL: `https://raw.githubusercontent.com/espressif/esp-idf/f935c17a958d6a8fe3194e743b3d526998341e50/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py` (last change of the file on branch `release/v4.4`)
  - SHA-256: `1e8f0dc9ff99e20d888fd5a9cab621e302acf55111c52dbe19ca92bf2bcd3020`
  - Used unmodified.
- **PlatformIO embedding:** `[env:esp32dev]` in `firmware/platformio.ini` sets `board_build.embed_files = data/cert/x509_crt_bundle.bin`, exposing linker symbol `_binary_data_cert_x509_crt_bundle_bin_start`.

## Refreshing the bundle

Prerequisites: Python 3 with `cryptography` and `certifi` installed (upgrade `certifi` in a temporary virtual environment when refreshing the root list).

The commands download the generator into a temporary folder, stop if its SHA-256 differs from the value above, and write the bundle into `firmware/data/cert/`.

From the repository root (Bash):

```bash
GEN_URL=https://raw.githubusercontent.com/espressif/esp-idf/f935c17a958d6a8fe3194e743b3d526998341e50/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py
GEN_SHA256=1e8f0dc9ff99e20d888fd5a9cab621e302acf55111c52dbe19ca92bf2bcd3020
TMP=$(mktemp -d)
curl -fsSL -o "$TMP/gen_crt_bundle.py" "$GEN_URL"
echo "$GEN_SHA256  $TMP/gen_crt_bundle.py" | sha256sum -c - && \
  (cd "$TMP" && python gen_crt_bundle.py --input "$(python -c 'import certifi; print(certifi.where())')") && \
  mkdir -p firmware/data/cert && \
  mv -f "$TMP/x509_crt_bundle" firmware/data/cert/x509_crt_bundle.bin
rm -rf "$TMP"
```

From the repository root (PowerShell):

```powershell
$genUrl = "https://raw.githubusercontent.com/espressif/esp-idf/f935c17a958d6a8fe3194e743b3d526998341e50/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py"
$genSha256 = "1e8f0dc9ff99e20d888fd5a9cab621e302acf55111c52dbe19ca92bf2bcd3020"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
Invoke-WebRequest -Uri $genUrl -OutFile "$tmp\gen_crt_bundle.py"
if ((Get-FileHash "$tmp\gen_crt_bundle.py" -Algorithm SHA256).Hash -ne $genSha256) { throw "gen_crt_bundle.py SHA-256 mismatch" }
$pem = python -c "import certifi; print(certifi.where())"
Push-Location $tmp
python gen_crt_bundle.py --input $pem
Pop-Location
New-Item -ItemType Directory -Force -Path firmware\data\cert | Out-Null
Move-Item -Force "$tmp\x509_crt_bundle" firmware\data\cert\x509_crt_bundle.bin
Remove-Item -Recurse -Force $tmp
```

After regenerating `firmware/data/cert/x509_crt_bundle.bin`, update the `certifi` version, certificate count, byte size, and SHA-256 digests in this file, then rebuild the `esp32dev` firmware.
