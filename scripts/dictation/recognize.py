"""One selected CPU model per process; no download or persistent loader."""
import os
import sys
import time
from pathlib import Path


def create(model, directory):
    import sherpa_onnx
    d = Path(directory)
    options = dict(num_threads=min(4, os.cpu_count() or 1), provider="cpu", debug=False)
    r = sherpa_onnx.OfflineRecognizer
    if model.startswith("moonshine-"):
        return r.from_moonshine(
            preprocessor=str(d / "preprocess.onnx"), encoder=str(d / "encode.int8.onnx"),
            uncached_decoder=str(d / "uncached_decode.int8.onnx"),
            cached_decoder=str(d / "cached_decode.int8.onnx"), tokens=str(d / "tokens.txt"), **options)
    if model == "parakeet-v3":
        return r.from_transducer(
            encoder=str(d / "encoder.int8.onnx"), decoder=str(d / "decoder.int8.onnx"),
            joiner=str(d / "joiner.int8.onnx"), tokens=str(d / "tokens.txt"),
            model_type="nemo_transducer", decoding_method="greedy_search", **options)
    if model == "qwen3-0.6b":
        return r.from_qwen3_asr(
            conv_frontend=str(d / "conv_frontend.onnx"), encoder=str(d / "encoder.int8.onnx"),
            decoder=str(d / "decoder.int8.onnx"), tokenizer=str(d / "tokenizer"),
            max_total_len=1024, max_new_tokens=256, **options)
    raise ValueError("Unsupported model")


def main():
    import resource
    import soundfile as sf
    model, directory, audio_path, output = sys.argv[1:]
    started = time.monotonic()
    recognizer = create(model, directory)
    loaded = time.monotonic()
    parts = []
    # Bounded audio windows avoid loading ten minutes of PCM or huge decoder caches.
    # Prefer a quiet boundary near 25 seconds. Never drop or overlap samples.
    import numpy as np
    with sf.SoundFile(audio_path) as audio:
        if audio.samplerate != 16000 or audio.channels != 1:
            raise ValueError("Expected mono 16 kHz audio")
        pending = np.empty(0, dtype=np.float32)
        while True:
            block = audio.read(30 * 16000 - len(pending), dtype="float32")
            samples = np.concatenate((pending, block))
            if not len(samples):
                break
            cut = len(samples)
            if len(samples) == 30 * 16000:
                positions = range(20 * 16000, 29 * 16000, 1600)
                cut = min(positions, key=lambda p: float(np.mean(samples[p:p+1600] ** 2))) + 800
            pending = samples[cut:]
            stream = recognizer.create_stream()
            stream.accept_waveform(16000, samples[:cut])
            recognizer.decode_stream(stream)
            parts.append(stream.result.text.strip())
            del stream
    Path(output).write_text(" ".join(p for p in parts if p), encoding="utf-8")
    print(f"Load: {loaded-started:.2f}s; decode: {time.monotonic()-loaded:.2f}s; "
          f"peak RAM: {resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB; CPU", file=sys.stderr)


if __name__ == "__main__":
    main()
