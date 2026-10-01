"""Create the synthetic, three-color clip used by movie_seek/startup.tjs.

The clip and packaged fixture belong in a build directory, not in Git.
"""

import argparse
import json
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        args.ffmpeg, "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "color=c=red:s=320x180:r=30:d=1",
        "-f", "lavfi", "-i", "color=c=green:s=320x180:r=30:d=1",
        "-f", "lavfi", "-i", "color=c=blue:s=320x180:r=30:d=1",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3",
        "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0[v]",
        "-map", "[v]", "-map", "3:a",
        "-c:v", "libx264", "-g", "15", "-keyint_min", "15",
        "-sc_threshold", "0", "-bf", "0", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-ac", "2", "-movflags", "+faststart",
        "-y", str(output),
    ]
    subprocess.run(command, check=True)

    probe = subprocess.run(
        [args.ffprobe, "-v", "error", "-show_entries",
         "format=duration:stream=codec_type,nb_frames,avg_frame_rate",
         "-of", "json", str(output)],
        check=True, capture_output=True, text=True,
    )
    info = json.loads(probe.stdout)
    video = next(stream for stream in info["streams"] if stream["codec_type"] == "video")
    audio = next(stream for stream in info["streams"] if stream["codec_type"] == "audio")
    if (int(video["nb_frames"]) != 90 or video["avg_frame_rate"] != "30/1" or
            abs(float(info["format"]["duration"]) - 3.0) > 0.005):
        raise RuntimeError("movie_seek requires exactly 90 frames at 30 fps and 3 seconds")
    print(f"{output} (90 frames, 30 fps, 3 seconds, audio={audio['codec_type']})")


if __name__ == "__main__":
    main()
