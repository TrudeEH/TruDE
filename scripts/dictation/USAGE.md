# Local dictation backends

Super+D records from the default PipeWire input, then stops and pastes the entire
transcript into the currently focused field. Configure models in Settings.
No audio uploads, automatic downloads, persistent inference server, or GPU loader.

The clipboard owner runs in a separate transient user service so it survives
recognition completion. Paste targets the focused Hyprland window explicitly,
using Ctrl+Shift+V for terminals and Ctrl+V elsewhere. It uses the Hyprland 0.55+ Lua dispatcher API and never presses Enter.
The original launcher and both powerline modules refresh immediately on
recording, transcription, and cleanup via the shared Waybar signal 9. The one-second poll
remains for screen-sharing detection and recovery from an unhandled worker kill.

## Models

- Moonshine Tiny/Base: English INT8 ONNX exports.
- Parakeet TDT 0.6B v3: INT8 ONNX, 25 European languages.
- Qwen3-ASR 0.6B: dated INT8 ONNX export, 30 languages plus Chinese dialects.
- Whisper Base/Small/Large v3 Turbo: whisper.cpp. Existing older selections remain
  supported and removable, but are not advertised in the picker.

Python is not a TruDE installation dependency. Selecting an ONNX model installs
python3-venv only if missing, after a separate sudo confirmation. Whisper setup
similarly installs missing CMake/build dependencies on demand. Existing Debian
packages are reused. Model deletion removes user-owned runtimes, not system
packages, which other applications may need.

The first three share a pinned sherpa-onnx CPU virtual environment. Only one model
is instantiated, after recording stops. The worker lock prevents concurrent tests
and recordings; the installation lock prevents deleting/loading files concurrently.
Inference exits after completion. ONNX computation uses at most four threads.
Whisper also uses four threads. No runtime is loaded merely by opening Settings.

The ONNX adapter processes bounded windows, choosing a low-energy boundary between
20 and 29 seconds. No audio is skipped or overlapped. This is not full VAD; words
crossing a boundary may be misrecognized. Test long dictation before relying on it.
The five-second test prints load/decode time and peak memory for ONNX backends.

Model archives are SHA-256 verified. Extracted test recordings and documentation
are excluded; model licenses are retained where supplied. Sources:
https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models
https://huggingface.co/ggerganov/whisper.cpp

Deletion is explicitly confirmed in Settings. It removes the chosen model and
removes the runtime when the last model using it is deleted. Other models remain
on disk, never loaded concurrently. Installers disable pip caching and remove
staging files on success, failure, or handled signals. Hard kills/power loss can
leave a setup directory; no unconditional startup deletion is performed.

Run `sh scripts/dictation/test.sh` for isolated controller/lifecycle tests.
