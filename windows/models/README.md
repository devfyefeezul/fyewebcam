# Models

`selfie_segmenter.onnx` is Google's MediaPipe selfie segmenter (256 x 256 RGB in [0, 1], NHWC; one
output channel with the person probability), converted to ONNX by `tools/convert_segmenter.py`. It
is embedded in fyewebcam.exe and drives the background blur.

Source: https://ai.google.dev/edge/mediapipe/solutions/vision/image_segmenter (Apache License 2.0).
